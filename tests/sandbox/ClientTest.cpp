//
// Dispatch-handling tests for the HTTP client: given what the peer sent,
// does the driver return the right thing?
//
// The real Client.c is compiled into usermode (see SandboxDriver.h), so
// the object under test is the shipping translation unit rather than a
// copy of it. Each test scripts a peer, issues a request, drains, and
// asserts on the answer the caller got, where the bytes landed, whether
// the connection was pooled or closed, and whether anything leaked.
//
// The interesting cases are all shapes a well-behaved server never
// produces -- which is exactly why they had never been exercised.
//

#include "SubtreeResponse.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "SandboxSocket.h"

extern BLORGFS_STATISTICS ShimStatistics;
}

namespace
{

struct ReadResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Bytes = 0;
    BOOLEAN HasVersion = FALSE;
    ULONG64 VersionSize = 0;
    ULONG64 VersionTime = 0;
};

ReadResult LastRead;

void OnFileRead(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    (void)CallerContext;

    LastRead.Calls++;
    LastRead.Status = Status;
    LastRead.Bytes = FileBuffer ? FileBuffer->BodyBufferSize : 0;
    LastRead.HasVersion = FileBuffer ? FileBuffer->HasVersion : FALSE;
    LastRead.VersionSize = FileBuffer ? FileBuffer->VersionSize : 0;
    LastRead.VersionTime = FileBuffer ? FileBuffer->VersionTime : 0;
}

struct FileInfoResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    DIRECTORY_ENTRY_METADATA Meta = {};
};

FileInfoResult LastFileInfo;

void OnFileInfo(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* FileInfo, PVOID CallerContext)
{
    (void)CallerContext;

    LastFileInfo.Calls++;
    LastFileInfo.Status = Status;

    if (FileInfo)
    {
        LastFileInfo.Meta = *FileInfo;
    }
}

struct ChangesResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PCHANGE_BATCH Batch = nullptr;
};

ChangesResult LastChanges;

struct DirInfoResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PDIRECTORY_INFO DirInfo = nullptr;
};

DirInfoResult LastDirInfo;

void OnDirInfo(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext)
{
    (void)CallerContext;

    LastDirInfo.Calls++;
    LastDirInfo.Status = Status;
    LastDirInfo.DirInfo = DirInfo;
}

void OnChanges(NTSTATUS Status, PCHANGE_BATCH Batch, PVOID CallerContext)
{
    (void)CallerContext;

    LastChanges.Calls++;
    LastChanges.Status = Status;
    LastChanges.Batch = Batch;
}

UNICODE_STRING MakePath(wchar_t* literal)
{
    UNICODE_STRING path;
    path.Buffer = literal;
    path.Length = (USHORT)(wcslen(literal) * sizeof(wchar_t));
    path.MaximumLength = path.Length;
    return path;
}

#define DELIVER(bytes) \
    { SandboxStepDeliver, (const unsigned char*)(bytes), sizeof(bytes) - 1, STATUS_SUCCESS, TRUE }

#define CLOSE_STEP \
    { SandboxStepClose, nullptr, 0, STATUS_SUCCESS, TRUE }

class HttpClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SandboxInitialize();
        LastRead = {};
        LastFileInfo = {};
        LastChanges = {};
        LastDirInfo = {};
    }

    //
    // Every test ends the same way: drain, then assert nothing leaked. A
    // leak in an error path is the most likely defect in code shaped like
    // this, and checking it per-test attributes the leak to the scenario
    // that caused it.
    //
    void TearDown() override
    {
        SandboxDrainCompletions();
        ShimDrainWorkItems();
        BlorgCleanupWskClient();
        BlorgFreeChangeBatch(LastChanges.Batch);
        BlorgReleaseDirectoryInfo(LastDirInfo.DirInfo);

        EXPECT_EQ(0u, ShimPoolOutstanding()) << "pool allocation(s) leaked";
    }

    //
    // A work item can issue more I/O (a retry's fresh connect does), so
    // this runs until neither side has anything left.
    //
    void Drain()
    {
        do
        {
            SandboxDrainCompletions();
        } while (ShimDrainWorkItems() > 0);
    }

    // Issues a ranged read against the current script.
    NTSTATUS Read(unsigned char* target, SIZE_T length, SIZE_T offset = 0)
    {
        Mdl = ShimCreateMdl(target, length);

        wchar_t path[] = L"/media/file.bin";
        UNICODE_STRING pathString = MakePath(path);

        return BlorgHttpGetFileMdl(&pathString, offset, length, Mdl, OnFileRead, nullptr);
    }

    //
    // Scripts a peer that answers with Headers and then Length bytes of
    // Body, delivered in one burst. Content-Length is the caller's to write,
    // so a test can lie in it. The response is kept on the fixture because
    // the script refers to it until the test drains.
    //
    void Respond(const char* Headers, const void* Body, SIZE_T Length)
    {
        SIZE_T headerLength = strlen(Headers);

        Response.assign(Headers, Headers + headerLength);
        Response.insert(Response.end(), C_CAST(const unsigned char*, Body), C_CAST(const unsigned char*, Body) + Length);

        Step = { SandboxStepDeliver, Response.data(), Response.size(), STATUS_SUCCESS, TRUE };
        SandboxSetPeerScript(&Step, 1);
    }

    void FreeMdl()
    {
        if (Mdl)
        {
            ShimFreeMdl(Mdl);
            Mdl = nullptr;
        }
    }

    PMDL Mdl = nullptr;
    std::vector<unsigned char> Response;
    SANDBOX_STEP Step = {};
};

///////////////////////////////////////////////////////////////////////////
// Dispatch handling -- the response the caller gets for what arrived
///////////////////////////////////////////////////////////////////////////

TEST_F(HttpClientTest, RangedReadSucceeds)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    ASSERT_EQ(STATUS_PENDING, Read(target, sizeof(target)));

    Drain();

    EXPECT_TRUE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(sizeof(target), LastRead.Bytes);
    EXPECT_EQ(0, memcmp(target, "ABCDEFGH", sizeof(target))) << "body did not land in the caller's MDL";
    EXPECT_EQ(1u, SandboxSocketsPooled()) << "a clean response should return its connection to the pool";
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// The disk cache keeps a fetched block only under the version the
// response's entity tag names, so the tag has to come back as the same
// size and 100-ns time since 1601 a listing reports: 0x5f5e1000 seconds
// since 1970 is 1600000000, and 0x3e8 nanoseconds is 10 ticks.
//
TEST_F(HttpClientTest, AFileReadCarriesTheVersionItsEntityTagNames)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nETag: \"5f5e1000.000003e8-2a\"\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    ASSERT_EQ(STATUS_PENDING, Read(target, sizeof(target)));

    Drain();

    EXPECT_TRUE(NT_SUCCESS(LastRead.Status));
    EXPECT_TRUE(LastRead.HasVersion);
    EXPECT_EQ(0x2Aull, LastRead.VersionSize);
    EXPECT_EQ(1600000000ull * 10000000 + 116444736000000000ull + 10, LastRead.VersionTime);

    FreeMdl();
}

//
// Weak, out of range, missing a part, or another server's spelling
// entirely: none names a version, and the read itself still succeeds. One
// pooled connection answers all four in turn.
//
TEST_F(HttpClientTest, AWeakOrForeignEntityTagNamesNoVersion)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nETag: W/\"5f5e1000.000003e8-2a\"\r\nContent-Length: 8\r\n\r\nABCDEFGH"),
        DELIVER("HTTP/1.1 206 Partial Content\r\nETag: \"5f5e1000.3b9aca00-2a\"\r\nContent-Length: 8\r\n\r\nABCDEFGH"),
        DELIVER("HTTP/1.1 206 Partial Content\r\nETag: \"5f5e1000-2a\"\r\nContent-Length: 8\r\n\r\nABCDEFGH"),
        DELIVER("HTTP/1.1 206 Partial Content\r\nETag: \"d41d8cd98f00b204e9800998ecf8427e\"\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    for (SIZE_T i = 0; i < RTL_NUMBER_OF(script); ++i)
    {
        unsigned char target[8] = {};

        LastRead = {};

        ASSERT_EQ(STATUS_PENDING, Read(target, sizeof(target)));

        Drain();

        EXPECT_TRUE(NT_SUCCESS(LastRead.Status)) << "response " << i;
        EXPECT_FALSE(LastRead.HasVersion) << "response " << i;

        FreeMdl();
    }

    EXPECT_EQ(1u, SandboxSocketsCreated()) << "every response should have come over the one pooled connection";
}

//
// The dangerous variant of the shape below: Content-Length agrees with the
// Range asked for -- so every length check the client makes is satisfied --
// but the peer then puts *more* body bytes than that on the wire, inside
// the same burst that carried the headers.
//
// In zero-copy mode the client drains headers into its own 2 KB-ish
// scratch buffer (grown a page at a time), and on the receive that finally
// completes the headers it copies whatever body bytes arrived alongside
// them -- the "spill" -- straight into the caller's MDL. That spill is
// sized from what *arrived*, so a peer that over-sends makes it exceed the
// caller's buffer, which is only as large as the range it asked for. The
// declared Content-Length being honest is exactly what gets the request
// past the earlier checks.
//
// The MDL here deliberately describes only the first 4 bytes of a much
// larger array, so anything written past the caller's buffer lands in the
// canary rather than in unrelated memory, and shows up as a deterministic
// assertion instead of depending on the allocator.
//
TEST_F(HttpClientTest, PeerSendingMoreBodyThanContentLengthMustNotOverrunTheCallerBuffer)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char canaried[512];
    memset(canaried, 0xAA, sizeof(canaried));

    const SIZE_T requested = 4;

    Read(canaried, requested);
    Drain();

    //
    // The driver's contract for a peer that sends more body than its
    // declared Content-Length is REJECTION: the response is a protocol
    // violation, reported as STATUS_INVALID_PARAMETER with the caller's
    // buffer left untouched. That is stronger than "did not overrun" --
    // these assertions equally catch a client that copies some or all of
    // the over-sent bytes before failing, which a canary alone would not.
    //
    ASSERT_EQ(1, LastRead.Calls);
    ASSERT_EQ(STATUS_INVALID_PARAMETER, LastRead.Status);

    for (SIZE_T i = 0; i < sizeof(canaried); ++i)
    {
        ASSERT_EQ(0xAA, canaried[i])
            << "byte " << i << " was overwritten -- byte 0..3 must survive "
               "unwritten too; a peer that over-sends its declared Content-Length "
               "must not reach the caller's MDL at all";
    }

    FreeMdl();
}

//
// A 206 whose Content-Length disagrees with the Range asked for. Checked
// independently of the general size ceiling, because a value can be well
// under the ceiling and still be wrong for this request -- a server that
// ignored Range and returned the whole file.
//
TEST_F(HttpClientTest, ContentLengthNotMatchingRangeIsRejected)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4096\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(0u, SandboxSocketsPooled()) << "a connection that misbehaved must not be pooled";
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// Rejected outright rather than truncated: truncating would let a peer
// make the client read a body shorter than what was actually sent, which
// desyncs a length-prefixed protocol on a keep-alive connection.
//
TEST_F(HttpClientTest, ContentLengthOverPolicyCeilingIsRejected)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 999999999999\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// A response with more headers than the parser was given room for is not
// truncated, it is rejected outright -- picohttpparser answers -1, the same
// as for a malformed status line, so the response looks broken rather than
// oversized. At 16 entries that was reachable by ordinary servers: a plain
// nginx 206 already spends five or six, and anything behind a CDN or
// carrying the usual security and CORS headers passes 16 without trying.
//
// Twenty filler headers around a valid 206, which fails on a 16-entry array
// and parses on the current one. The body still has to arrive intact, since
// the point is that the response is USED, not merely accepted.
//
TEST_F(HttpClientTest, ResponseWithManyHeadersIsStillParsed)
{
    std::string response = "HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n";

    for (int i = 0; i < 20; ++i)
    {
        response += "X-Filler-" + std::to_string(i) + ": v\r\n";
    }

    response += "\r\nABCDEFGH";

    const SANDBOX_STEP script[] =
    {
        { SandboxStepDeliver, (const unsigned char*)response.data(), response.size(), STATUS_SUCCESS, TRUE }
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status)
        << "a well-formed response was rejected for carrying more headers than the array held";
    EXPECT_EQ(0, memcmp(target, "ABCDEFGH", sizeof(target)))
        << "the body must survive a header set that fills more of the array";

    FreeMdl();
}

//
// Two Content-Length headers. Taking the first and ignoring the second is
// how a client ends up framing a response differently from whatever proxy
// or origin produced it -- and because this driver pools keep-alive
// connections, the bytes it did not consume do not vanish, they become the
// head of the next response read on the same socket. That is response
// smuggling, read from the client side: one request's body served as
// another's answer.
//
// Both orderings are checked. A short-then-long pair leaves the tail on the
// wire; long-then-short over-reads into whatever follows. Neither may be
// accepted, so the assertion is the status rather than the byte count.
//
TEST_F(HttpClientTest, DuplicateContentLengthIsRejected)
{
    static const SANDBOX_STEP shortFirst[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    static const SANDBOX_STEP longFirst[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\nContent-Length: 4\r\n\r\nABCDEFGH")
    };

    const SANDBOX_STEP* const scripts[] = { shortFirst, longFirst };

    for (const SANDBOX_STEP* script : scripts)
    {
        SandboxSetPeerScript(script, 1);

        LastRead = {};

        unsigned char target[8] = {};

        Read(target, sizeof(target));
        Drain();

        EXPECT_EQ(1, LastRead.Calls);
        EXPECT_EQ(STATUS_INVALID_NETWORK_RESPONSE, LastRead.Status)
            << "a response declaring its own length twice must not be framed by either value";

        FreeMdl();
    }
}

//
// The pool-exhaustion shape: a peer that sends header bytes and never
// terminates them. picohttpparser answers "incomplete" for as long as this
// goes on, which is the signal that makes the client post another receive
// and grow its NonPagedPoolNx buffer again -- so before HTTP_MAX_HEADER_BYTES
// the only limit was HttpGrowBufferIfNeeded's MAXULONG, close to 4 GB of
// non-paged pool per in-flight request from a peer that has sent no valid
// response at all.
//
// One unterminated header line rather than many short ones, because many
// short ones hit HTTP_MAX_HEADERS first and fail as a parse error -- a
// different bound that was already there, and not the one under test.
//
// The exact status is the assertion. A run without the cap also fails this
// request eventually, once the peer runs out of script, so "did it fail"
// does not distinguish the two; STATUS_INVALID_NETWORK_RESPONSE is reachable
// only through the ceiling.
//
TEST_F(HttpClientTest, UnterminatedHeadersAreRejectedRatherThanGrownWithoutLimit)
{
    std::string flood = "HTTP/1.1 206 Partial Content\r\nX-Endless: ";
    flood.append(256 * 1024, 'a');

    const SANDBOX_STEP script[] =
    {
        { SandboxStepDeliver, (const unsigned char*)flood.data(), flood.size(), STATUS_SUCCESS, TRUE }
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_INVALID_NETWORK_RESPONSE, LastRead.Status)
        << "unterminated headers must be refused at the ceiling, not grown into";

    FreeMdl();
}

//
// The peer closes mid-body. Response bytes were already consumed, so this
// is NOT the idle-close race and must not be retried -- a retry would
// re-read a partial body onto itself.
//
TEST_F(HttpClientTest, TruncatedBodyFailsWithoutRetry)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 16\r\n\r\nABCD"),
        CLOSE_STEP
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[16] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(1u, SandboxSocketsCreated()) << "retried after consuming response bytes";
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// 404 maps to a specific status, not a generic failure: that is what lets
// the create path cache a negative result instead of surfacing a
// confusing error to the caller.
//
TEST_F(HttpClientTest, NotFoundMapsToObjectNameNotFound)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, LastRead.Status);
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// 403 is the server refusing the path (host permissions, or a path that
// escapes its root): the caller should see "access denied", not "the
// parameter is incorrect".
//
TEST_F(HttpClientTest, ForbiddenMapsToAccessDenied)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(static_cast<NTSTATUS>(STATUS_ACCESS_DENIED), LastRead.Status);
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// 416 means the read started at or past the file's current end on the
// server -- the file shrank under a cached size. That is end of file.
//
TEST_F(HttpClientTest, RangeNotSatisfiableMapsToEndOfFile)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(static_cast<NTSTATUS>(STATUS_END_OF_FILE), LastRead.Status);
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

TEST_F(HttpClientTest, MalformedStatusLineIsRejected)
{
    static const SANDBOX_STEP script[] = { DELIVER("NOT-HTTP AT ALL\r\n\r\n") };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// No Content-Length and no chunked support means no framing at all. The
// client has to fail rather than guess how much to read.
//
TEST_F(HttpClientTest, MissingContentLengthIsRejected)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nServer: x\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

///////////////////////////////////////////////////////////////////////////
// Reassembly and reentrancy
///////////////////////////////////////////////////////////////////////////

//
// A response dribbled in six pieces, with the stack budget squeezed so
// the expand-and-continue path is on the table. This is the shape the
// client's stack-safety logic exists for.
//
TEST_F(HttpClientTest, DribbledResponseReassembles)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Par"),
        DELIVER("tial Content\r\nCont"),
        DELIVER("ent-Length: 8\r\n"),
        DELIVER("\r\n"),
        DELIVER("ABCD"),
        DELIVER("EFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    ShimSetRemainingStack(4096);

    unsigned char target[8] = {};

    Read(target, sizeof(target));
    Drain();

    EXPECT_TRUE(NT_SUCCESS(LastRead.Status));
    EXPECT_EQ(0, memcmp(target, "ABCDEFGH", sizeof(target)));
    EXPECT_EQ(1, LastRead.Calls);

    FreeMdl();
}

//
// The keep-alive idle-close race: a pooled connection the peer already
// dropped. A close before any response byte is retryable exactly once on
// a fresh connection -- without that, every keep-alive race would surface
// as a user-visible read failure.
//
// The stale connection needs no scripted CLOSE: its warmup script is
// exhausted after the first read, and the peer treats an exhausted script
// as a close -- which is classified identically to an idle-close for
// retry purposes. Scripts bind at socket CREATION, so the step list set
// before the second read is inherited only by the RETRY's fresh socket;
// putting a CLOSE_STEP there would close the retry connection instead of
// the stale one, which is exactly what an earlier version of this test
// did without noticing.
//
TEST_F(HttpClientTest, IdleClosedPooledConnectionIsRetriedOnce)
{
    static const SANDBOX_STEP warmup[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWARM")
    };

    SandboxSetPeerScript(warmup, RTL_NUMBER_OF(warmup));

    unsigned char first[4] = {};
    Read(first, sizeof(first));
    Drain();

    ASSERT_EQ(1u, SandboxSocketsPooled());
    FreeMdl();

    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nGOOD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    LastRead = {};
    ULONG createdBefore = SandboxSocketsCreated();

    unsigned char second[4] = {};
    Read(second, sizeof(second));
    Drain();

    EXPECT_GT(SandboxSocketsCreated(), createdBefore) << "the retry did not open a fresh connection";
    EXPECT_EQ(1, LastRead.Calls);

    //
    // A fresh connection plus a callback proves the retry machinery ran,
    // not that it worked: only the delivered status and bytes say whether
    // the retried read actually succeeded or failed after reconnecting.
    //
    ASSERT_EQ(STATUS_SUCCESS, LastRead.Status)
        << "the retry reconnected but the read still failed";
    ASSERT_EQ(sizeof(second), LastRead.Bytes);
    EXPECT_EQ(0, memcmp(second, "GOOD", sizeof(second)))
        << "the retried read delivered wrong body bytes";

    FreeMdl();
}

//
// A pooled connection that never completed a handshake -- a pre-warmed one,
// which enters the pool straight from its connect -- may have been dropped
// by a server that times out silent clients, and its handshake then fails.
// That is the idle-close race again, so it is retried once on a fresh
// connection. The stub fails both handshakes, which pins the one retry: a
// second fresh connection, or a success, would mean a retry loop or none.
//
TEST_F(HttpClientTest, APooledConnectionWhoseHandshakeFailsIsRetriedOnceOnAFreshOne)
{
    static const SANDBOX_STEP warmup[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWARM")
    };

    SandboxSetPeerScript(warmup, RTL_NUMBER_OF(warmup));

    unsigned char first[4] = {};
    Read(first, sizeof(first));
    Drain();

    ASSERT_EQ(1u, SandboxSocketsPooled());
    FreeMdl();

    global.TlsEnabled = TRUE;
    SandboxFailNextHandshakesWith(2, STATUS_CONNECTION_RESET);

    LastRead = {};
    ULONG createdBefore = SandboxSocketsCreated();

    unsigned char second[4] = {};
    Read(second, sizeof(second));
    Drain();

    EXPECT_EQ(createdBefore + 1, SandboxSocketsCreated()) << "the retry must open exactly one fresh connection";
    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_CONNECTION_RESET, LastRead.Status);

    FreeMdl();
}

//
// On a fresh connection a failed handshake is the server's answer, not a
// stale socket, so it fails the read at once.
//
TEST_F(HttpClientTest, AFreshConnectionWhoseHandshakeFailsIsNotRetried)
{
    global.TlsEnabled = TRUE;
    SandboxFailNextHandshakesWith(1, STATUS_CONNECTION_RESET);

    unsigned char target[4] = {};
    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(1u, SandboxSocketsCreated());
    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_CONNECTION_RESET, LastRead.Status);

    FreeMdl();
}

//
// A connect that times out is replaced by a new one rather than failing the
// read. The connect watchdog used to be a single 15 s attempt, and one lost
// connect cost a reader all of it (a measured 15,001 ms app read). The
// sandbox completes acquisitions inline, so these pin the client's decision
// -- retry on STATUS_IO_TIMEOUT, how many times, and nothing else -- not
// the watchdog timing, which lives in Socket.c. HTTP_CONNECT_ATTEMPTS is
// 4: three timeouts are survivable, a fourth is not.
//
TEST_F(HttpClientTest, TimedOutConnectIsRetriedOnANewSocket)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nGOOD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    SandboxFailNextAcquiresWith(3, STATUS_IO_TIMEOUT);

    const ULONG64 retries = ShimStatistics.ConnectRetries;

    unsigned char target[4] = {};
    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(1, LastRead.Calls);
    ASSERT_EQ(STATUS_SUCCESS, LastRead.Status)
        << "a timed-out connect failed the read instead of being retried";
    EXPECT_EQ(0, memcmp(target, "GOOD", sizeof(target)));
    EXPECT_EQ(retries + 3, ShimStatistics.ConnectRetries);

    FreeMdl();
}

//
// The budget is what keeps a dead backend failing in bounded time. The
// follow-up read proves the failed one stopped after exactly four
// attempts: had it made fewer, a scripted failure would still be pending
// and would fail the second read too.
//
TEST_F(HttpClientTest, ConnectThatKeepsTimingOutFailsAfterTheAttemptBudget)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nGOOD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    SandboxFailNextAcquiresWith(4, STATUS_IO_TIMEOUT);

    unsigned char first[4] = {};
    Read(first, sizeof(first));
    Drain();

    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_IO_TIMEOUT, LastRead.Status)
        << "four timed-out connects must fail the read, not retry forever";
    FreeMdl();

    LastRead = {};
    unsigned char second[4] = {};
    Read(second, sizeof(second));
    Drain();

    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status)
        << "the failed read gave up before using all four attempts";
    FreeMdl();
}

//
// Only a timeout is retried. A refused or reset connect is an answer from
// the peer, and asking again at once would get the same one, so it must
// fail on the first attempt as it always has.
//
TEST_F(HttpClientTest, RefusedConnectIsNotRetried)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nGOOD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    SandboxFailNextAcquiresWith(1, STATUS_CONNECTION_REFUSED);

    unsigned char target[4] = {};
    Read(target, sizeof(target));
    Drain();

    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_CONNECTION_REFUSED, LastRead.Status);

    FreeMdl();
}

///////////////////////////////////////////////////////////////////////////
// Request shaping
///////////////////////////////////////////////////////////////////////////

//
// The URL encoding and the Range arithmetic are the two places a silent
// off-by-one would send a subtly wrong request and still parse the reply
// happily -- so the bytes on the wire are asserted directly.
//
TEST_F(HttpClientTest, RequestLineAndRangeAreWellFormed)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nABCD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[4] = {};

    Mdl = ShimCreateMdl(target, sizeof(target));

    wchar_t path[] = L"/a b/c.bin";
    UNICODE_STRING pathString = MakePath(path);

    BlorgHttpGetFileMdl(&pathString, 100, sizeof(target), Mdl, OnFileRead, nullptr);

    Drain();

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);

    ASSERT_GT(sentLength, 0u);
    EXPECT_NE(nullptr, strstr(text, "GET /get_file?path=")) << "request line does not target get_file";
    EXPECT_NE(nullptr, strstr(text, "%2Fa%20b%2Fc.bin")) << "path was not percent-encoded";
    EXPECT_NE(nullptr, strstr(text, "Range: bytes=100-103")) << "Range must be inclusive of the last byte";
    EXPECT_NE(nullptr, strstr(text, "Connection: keep-alive"));

    FreeMdl();
}

//
// Non-ASCII in a path, which is where the encoding actually has to be
// UTF-8 rather than merely "not ASCII-clean". Each of these characters is
// two UTF-8 bytes, so each becomes two percent-escapes -- a byte-per-byte
// encoder gets this right and a character-per-character one does not.
//
// Worth pinning independently of the space/slash case above because the
// encoder writes ANSI directly now instead of building UTF-16 for the
// formatter to narrow again. The wire bytes are the only place that
// change is observable, and they must not have moved.
//
TEST_F(HttpClientTest, NonAsciiPathIsPercentEncodedFromItsUtf8Bytes)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nABCD")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[4] = {};

    Mdl = ShimCreateMdl(target, sizeof(target));

    wchar_t path[] = L"/m\u00E9dia/\u00FCn\u00EFcode.bin";
    UNICODE_STRING pathString = MakePath(path);

    BlorgHttpGetFileMdl(&pathString, 0, sizeof(target), Mdl, OnFileRead, nullptr);

    Drain();

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);

    ASSERT_GT(sentLength, 0u);
    EXPECT_NE(nullptr, strstr(text, "path=%2Fm%C3%A9dia%2F%C3%BCn%C3%AFcode.bin"))
        << "multi-byte characters must be escaped one UTF-8 byte at a time";

    FreeMdl();
}

//
// The buffer-mode twin of PeerSendingMoreBodyThanContentLength..., and a
// far worse one: there the over-send is caught before anything is copied,
// here it is copied first and checked afterwards.
//
// A metadata response (file-info/dir-info, not a file read) has its body
// deserialized by flatcc, which requires 8-byte alignment, so when the
// headers end on an odd offset HttpReadResponse slides the body down to
// the next multiple of 8. The slide's length is "everything received past
// the headers" -- what the peer actually sent -- while the buffer was
// grown only to hold the aligned offset plus what the peer *declared*.
// A peer that declares a small Content-Length and then sends enough body
// to fill the receive buffer therefore slides bytes off the end of the
// pool block. The "server sent data beyond declared Content-Length" test
// that would reject this response runs after the slide, not before it.
//
// The response is built to land exactly on the boundary: headers of a
// length congruent to 1 mod 8 (the maximum 7-byte slide) followed by
// enough body to fill the file-info context's whole initial capacity, so
// the move ends 7 bytes past the allocation and lands in the shim pool's
// tail guard rather than in whatever the allocator put next.
//
TEST_F(HttpClientTest, OverSentMetadataBodyMustNotSlidePastTheReceiveBuffer)
{
    const char* headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n"
        "X: aaaaaa\r\n"
        "\r\n";

    const SIZE_T headerLength = strlen(headers);

    ASSERT_EQ(1u, headerLength % 8)
        << "this scenario needs headers ending 1 past a multiple of 8 so the "
           "body slides the maximum 7 bytes";

    //
    // PAGE_SIZE is the initial receive capacity BlorgHttpGetFileInformation
    // asks for, and the header-phase receive posts all of it, so a single
    // burst of exactly this size leaves Length == Capacity with the headers
    // still unaligned.
    //
    std::vector<unsigned char> response(PAGE_SIZE);
    memcpy(response.data(), headers, headerLength);
    memset(response.data() + headerLength, 'B', response.size() - headerLength);

    const SANDBOX_STEP script[] =
    {
        { SandboxStepDeliver, response.data(), response.size(), STATUS_SUCCESS, TRUE }
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    wchar_t path[] = L"/media/file.bin";
    UNICODE_STRING pathString = MakePath(path);

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetFileInformation(&pathString, OnFileInfo, nullptr));

    Drain();

    EXPECT_EQ(1, LastFileInfo.Calls);
    EXPECT_FALSE(NT_SUCCESS(LastFileInfo.Status))
        << "a body longer than the declared Content-Length must be rejected";
}

///////////////////////////////////////////////////////////////////////////
// The change feed and no-store
///////////////////////////////////////////////////////////////////////////

//
// A ChangeBatch as server-rs encodes one (bytes produced by its schema with
// the reference flatbuffers builder): epoch 7, generation 42, modified
// "media/a.bin" and "" (the root), created "media/new dir" and "m\u00E9dia",
// removed "old.bin". Paths are the server's keys -- relative, '/'-separated
// UTF-8 -- which is what the conversion below is checked against.
//
static const char kChangeBatch[] =
    "\x18\x00\x00\x00\x00\x00\x00\x00\x10\x00\x24\x00\x18\x00\x10\x00"
    "\x00\x00\x0c\x00\x08\x00\x04\x00\x10\x00\x00\x00\x20\x00\x00\x00"
    "\x30\x00\x00\x00\x58\x00\x00\x00\x2a\x00\x00\x00\x00\x00\x00\x00"
    "\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00"
    "\x04\x00\x00\x00\x07\x00\x00\x00\x6f\x6c\x64\x2e\x62\x69\x6e\x00"
    "\x02\x00\x00\x00\x14\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00"
    "\x6d\xc3\xa9\x64\x69\x61\x00\x00\x0d\x00\x00\x00\x6d\x65\x64\x69"
    "\x61\x2f\x6e\x65\x77\x20\x64\x69\x72\x00\x00\x00\x02\x00\x00\x00"
    "\x10\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x0b\x00\x00\x00\x6d\x65\x64\x69\x61\x2f\x61\x2e\x62\x69\x6e\x00";

//
// A reset (epoch 9, generation 3) that also lists a path, which a reset
// never needs: the driver drops everything on one anyway.
//
static const char kResetBatch[] =
    "\x14\x00\x00\x00\x10\x00\x24\x00\x1c\x00\x14\x00\x13\x00\x0c\x00"
    "\x08\x00\x04\x00\x10\x00\x00\x00\x20\x00\x00\x00\x20\x00\x00\x00"
    "\x20\x00\x00\x00\x00\x00\x00\x01\x03\x00\x00\x00\x00\x00\x00\x00"
    "\x09\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x78\x00\x00\x00";

//
// A DirectoryEntryMetadata for a 4096-byte file, encoded the same way.
//
static const char kFileInfo[] =
    "\x14\x00\x00\x00\x00\x00\x00\x00\x0c\x00\x24\x00\x1c\x00\x14\x00"
    "\x0c\x00\x04\x00\x0c\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x10\x00\x00\x00\x00\x00\x00";

std::wstring EntryPath(const CHANGE_ENTRY& Entry)
{
    return std::wstring(Entry.Path.Buffer, Entry.Path.Length / sizeof(WCHAR));
}

//
// The poll's request line carries where the follower is, and the answer
// comes back as this volume spells paths: a leading backslash, backslashes
// between components, UTF-16 from the server's UTF-8, and the root as a
// lone backslash. Spelled any other way a change would invalidate a key no
// cache entry has, and be silently lost.
//
TEST_F(HttpClientTest, ChangeBatchArrivesInTheVolumesOwnSpelling)
{
    char headers[128];
    sprintf_s(headers, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", sizeof(kChangeBatch) - 1);
    Respond(headers, kChangeBatch, sizeof(kChangeBatch) - 1);

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetChanges(7, 41, OnChanges, nullptr));

    Drain();

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);
    ASSERT_GT(sentLength, 0u);
    EXPECT_NE(nullptr, strstr(text, "GET /get_changes?epoch=7&since=41 HTTP/1.1\r\n"));

    ASSERT_EQ(1, LastChanges.Calls);
    ASSERT_EQ(STATUS_SUCCESS, LastChanges.Status);
    ASSERT_NE(nullptr, LastChanges.Batch);

    const CHANGE_BATCH* batch = LastChanges.Batch;
    EXPECT_EQ(7u, batch->Epoch);
    EXPECT_EQ(42u, batch->Generation);
    EXPECT_FALSE(batch->Reset);
    ASSERT_EQ(5u, batch->Count);

    EXPECT_EQ(L"\\media\\a.bin", EntryPath(batch->Entries[0]));
    EXPECT_EQ(ChangeModified, batch->Entries[0].Kind);
    EXPECT_EQ(L"\\", EntryPath(batch->Entries[1]));
    EXPECT_EQ(ChangeModified, batch->Entries[1].Kind);
    EXPECT_EQ(L"\\media\\new dir", EntryPath(batch->Entries[2]));
    EXPECT_EQ(ChangeCreated, batch->Entries[2].Kind);
    EXPECT_EQ(L"\\m\u00E9dia", EntryPath(batch->Entries[3]));
    EXPECT_EQ(ChangeCreated, batch->Entries[3].Kind);
    EXPECT_EQ(L"\\old.bin", EntryPath(batch->Entries[4]));
    EXPECT_EQ(ChangeRemoved, batch->Entries[4].Kind);
}

//
// A reset is acted on wholesale, so whatever paths it lists are not
// delivered: the follower would only spend time invalidating entries the
// reset drops anyway.
//
TEST_F(HttpClientTest, ResetBatchCarriesNoEntries)
{
    char headers[128];
    sprintf_s(headers, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", sizeof(kResetBatch) - 1);
    Respond(headers, kResetBatch, sizeof(kResetBatch) - 1);

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetChanges(0, 0, OnChanges, nullptr));

    Drain();

    ASSERT_EQ(STATUS_SUCCESS, LastChanges.Status);
    ASSERT_NE(nullptr, LastChanges.Batch);
    EXPECT_TRUE(LastChanges.Batch->Reset);
    EXPECT_EQ(9u, LastChanges.Batch->Epoch);
    EXPECT_EQ(3u, LastChanges.Batch->Generation);
    EXPECT_EQ(0u, LastChanges.Batch->Count);
}

//
// A batch past CHANGE_BATCH_MAX_ENTRIES is delivered as a reset rather than
// allocated in full or failed. Failing it would take the feed down and leave
// the caches on their short TTL until the server answered something smaller,
// which a burst that large never makes it do.
//
// The buffer is built by hand because no literal of that size is worth
// embedding: one vector of CHANGE_BATCH_MAX_ENTRIES + 1 offsets that all
// name the same empty string, which the format allows and the verifier
// accepts.
//
TEST_F(HttpClientTest, BatchPastTheEntryCapArrivesAsAReset)
{
    const ULONG count = CHANGE_BATCH_MAX_ENTRIES + 1;
    const SIZE_T vectorAt = 24;
    const SIZE_T stringAt = vectorAt + 4 + (4 * C_CAST(SIZE_T, count));
    std::vector<unsigned char> body(stringAt + 8);

    auto put16 = [&body](SIZE_T At, USHORT Value) { memcpy(&body[At], &Value, sizeof(Value)); };
    auto put32 = [&body](SIZE_T At, ULONG Value) { memcpy(&body[At], &Value, sizeof(Value)); };

    put32(0, 16);
    put16(4, 12);
    put16(6, 8);
    put16(14, 4);
    put32(16, 12);
    put32(20, C_CAST(ULONG, vectorAt - 20));
    put32(vectorAt, count);

    for (ULONG i = 0; i < count; ++i)
    {
        SIZE_T at = vectorAt + 4 + (4 * C_CAST(SIZE_T, i));
        put32(at, C_CAST(ULONG, stringAt - at));
    }

    char headers[128];
    sprintf_s(headers, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", body.size());
    Respond(headers, body.data(), body.size());

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetChanges(7, 1, OnChanges, nullptr));

    Drain();

    ASSERT_EQ(STATUS_SUCCESS, LastChanges.Status);
    ASSERT_NE(nullptr, LastChanges.Batch);
    EXPECT_TRUE(LastChanges.Batch->Reset);
    EXPECT_EQ(0u, LastChanges.Batch->Count);
}

//
// A feed that is not live answers 503, which must reach the follower as a
// failure so it takes the feed down rather than trusting the caches.
//
TEST_F(HttpClientTest, FeedThatIsNotLiveFailsThePoll)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetChanges(7, 1, OnChanges, nullptr));

    Drain();

    ASSERT_EQ(1, LastChanges.Calls);
    EXPECT_FALSE(NT_SUCCESS(LastChanges.Status));
    EXPECT_EQ(nullptr, LastChanges.Batch);
}

//
// Cache-Control: no-store is what the server sends for an answer it could
// not vouch for against its own feed, and NoStore is how the path cache
// learns not to keep it. The directive is found case-insensitively among
// others, on any Cache-Control header, and not mistaken for a longer token
// that merely starts with it. One fixture per case, because a pooled
// connection keeps its place in the script it was acquired under.
//
struct NoStoreCase
{
    const char* Header;
    BOOLEAN NoStore;
};

class HttpClientNoStoreTest : public HttpClientTest,
                              public ::testing::WithParamInterface<NoStoreCase>
{
};

TEST_P(HttpClientNoStoreTest, DirectiveMarksTheAnswer)
{
    char headers[256];
    sprintf_s(headers, "HTTP/1.1 200 OK\r\n%sContent-Length: %zu\r\n\r\n", GetParam().Header, sizeof(kFileInfo) - 1);
    Respond(headers, kFileInfo, sizeof(kFileInfo) - 1);

    wchar_t path[] = L"/media/file.bin";
    UNICODE_STRING pathString = MakePath(path);

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetFileInformation(&pathString, OnFileInfo, nullptr));

    Drain();

    ASSERT_EQ(1, LastFileInfo.Calls);
    ASSERT_EQ(STATUS_SUCCESS, LastFileInfo.Status);
    EXPECT_EQ(4096u, LastFileInfo.Meta.Size);
    EXPECT_EQ(GetParam().NoStore, LastFileInfo.Meta.NoStore);
}

INSTANTIATE_TEST_SUITE_P(
    CacheControlSpellings,
    HttpClientNoStoreTest,
    ::testing::Values(
        NoStoreCase{ "", FALSE },
        NoStoreCase{ "Cache-Control: no-store\r\n", TRUE },
        NoStoreCase{ "cache-control: max-age=0, No-Store\r\n", TRUE },
        NoStoreCase{ "Cache-Control: private\r\nCache-Control: no-store\r\n", TRUE },
        NoStoreCase{ "Cache-Control: no-store-later, no-cache\r\n", FALSE }));

///////////////////////////////////////////////////////////////////////////
// Subtree answers
///////////////////////////////////////////////////////////////////////////

//
// kSubtreeOutOfOrder with a fourth that names subdirectory 1 of a, which has
// only one.
//
static const char kSubtreeOutOfRange[] =
    "\x04\x00\x00\x00\x82\xff\xff\xff\x0c\x00\x00\x00\x28\x00\x00\x00"
    "\x18\x00\x00\x00\x04\x00\x00\x00\x40\x01\x00\x00\xf4\x00\x00\x00"
    "\xb0\x00\x00\x00\x68\x00\x00\x00\x02\x00\x00\x00\x20\x00\x00\x00"
    "\x0c\x00\x00\x00\x01\x00\x00\x00\x2c\x00\x00\x00\xb6\xfe\xff\xff"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x62\x00\x00\x00\xc6\xfe\xff\xff"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x61\x00\x00\x00\x08\x00\x14\x00"
    "\x10\x00\x04\x00\x08\x00\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00\x72\x2e\x62\x69"
    "\x6e\x00\x0a\x00\x10\x00\x0c\x00\x08\x00\x04\x00\x0a\x00\x00\x00"
    "\x0c\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x80\xff\xff\xff"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x04\x00\x00\x00\x10\xff\xff\xff"
    "\x00\x10\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00"
    "\x64\x2e\x62\x69\x6e\x00\x0a\x00\x0c\x00\x08\x00\x00\x00\x04\x00"
    "\x0a\x00\x00\x00\x08\x00\x00\x00\x01\x00\x00\x00\xc0\xff\xff\xff"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x04\x00\x00\x00\x50\xff\xff\xff"
    "\x00\x10\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00"
    "\x63\x2e\x62\x69\x6e\x00\x0a\x00\x0c\x00\x00\x00\x08\x00\x04\x00"
    "\x0a\x00\x00\x00\x10\x00\x00\x00\x01\x00\x00\x00\x08\x00\x08\x00"
    "\x00\x00\x04\x00\x08\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00"
    "\x04\x00\x00\x00\x98\xff\xff\xff\x00\x10\x00\x00\x00\x00\x00\x00"
    "\x04\x00\x00\x00\x05\x00\x00\x00\x62\x2e\x62\x69\x6e\x00\x0a\x00"
    "\x08\x00\x00\x00\x00\x00\x04\x00\x0a\x00\x00\x00\x0c\x00\x00\x00"
    "\x08\x00\x0c\x00\x08\x00\x04\x00\x08\x00\x00\x00\x10\x00\x00\x00"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x14\x00\x00\x00\x01\x00\x00\x00"
    "\x24\x00\x00\x00\x00\x00\x06\x00\x08\x00\x04\x00\x06\x00\x00\x00"
    "\x04\x00\x00\x00\x01\x00\x00\x00\x63\x00\x00\x00\x08\x00\x10\x00"
    "\x0c\x00\x04\x00\x08\x00\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00"
    "\x04\x00\x00\x00\x05\x00\x00\x00\x61\x2e\x62\x69\x6e\x00\x00\x00";

class HttpClientSubtreeTest : public HttpClientTest
{
protected:
    //
    // Asks for /r's listing with SubtreeEntries and answers with Body.
    // Returns the listing, which the fixture releases.
    //
    PDIRECTORY_INFO List(ULONG SubtreeEntries, const char* Body, SIZE_T Length)
    {
        char headers[128];
        sprintf_s(headers, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", Length);
        Respond(headers, Body, Length);

        wchar_t path[] = L"/r";
        UNICODE_STRING pathString = MakePath(path);

        EXPECT_EQ(STATUS_PENDING, BlorgHttpGetDirectoryInfo(&pathString, SubtreeEntries, OnDirInfo, nullptr));

        Drain();

        EXPECT_EQ(1, LastDirInfo.Calls);
        EXPECT_EQ(STATUS_SUCCESS, LastDirInfo.Status);
        return LastDirInfo.DirInfo;
    }
};

//
// Each listing is attached in the answer's order with the parent and
// subdirectory it names, and decoding stops at the first descendant whose
// parent is not yet decoded: what came before is still every listing
// nearer the root than the rest.
//
TEST_F(HttpClientSubtreeTest, ListingsArriveInOrderUpToAParentNotYetDecoded)
{
    PDIRECTORY_INFO root = List(64, kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);
    ASSERT_NE(nullptr, root);

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);
    ASSERT_GT(sentLength, 0u);
    EXPECT_NE(nullptr, strstr(text, "&subtree=64 HTTP/1.1\r\n"));

    ASSERT_EQ(2u, root->SubDirCount);
    ASSERT_EQ(3u, root->DescendantCount);

    const DIRECTORY_DESCENDANT* descendants = root->Descendants;
    EXPECT_EQ(0u, descendants[0].Parent);
    EXPECT_EQ(0u, descendants[0].SubDir);
    EXPECT_EQ(0u, descendants[1].Parent);
    EXPECT_EQ(1u, descendants[1].SubDir);
    EXPECT_EQ(1u, descendants[2].Parent);
    EXPECT_EQ(0u, descendants[2].SubDir);

    PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(descendants[2].Listing, 0);
    EXPECT_EQ(L"c.bin", std::wstring(file->Name, file->NameLength));
    EXPECT_EQ(nullptr, descendants[2].Listing->Descendants);
}

TEST_F(HttpClientSubtreeTest, ADescendantNamingASubdirectoryItsParentLacksEndsTheDecode)
{
    PDIRECTORY_INFO root = List(64, kSubtreeOutOfRange, sizeof(kSubtreeOutOfRange) - 1);
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(3u, root->DescendantCount);
}

//
// SubtreeEntries bounds what is decoded as well as what is asked for: a
// listing that would take the total past it is not attached. a's two
// entries and b's one fit in three; c's would make four.
//
TEST_F(HttpClientSubtreeTest, ListingsPastTheEntriesAskedForAreNotDecoded)
{
    PDIRECTORY_INFO root = List(3, kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(2u, root->DescendantCount);
}

//
// A listing asked for without its subtree (a refresh) keeps none the
// server sent anyway.
//
TEST_F(HttpClientSubtreeTest, ARequestThatAskedForNoSubtreeDecodesNone)
{
    PDIRECTORY_INFO root = List(0, kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);
    ASSERT_NE(nullptr, root);

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);
    ASSERT_GT(sentLength, 0u);
    EXPECT_EQ(nullptr, strstr(text, "subtree="));

    EXPECT_EQ(2u, root->SubDirCount);
    EXPECT_EQ(0u, root->DescendantCount);
    EXPECT_EQ(nullptr, root->Descendants);
}

//
// The subtree query is written after the encoded path in the room the
// encoder left, so the path keeps its escapes and the suffix follows the
// last of them.
//
TEST_F(HttpClientSubtreeTest, TheSubtreeQueryFollowsTheEncodedPath)
{
    char headers[128];
    sprintf_s(headers, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", sizeof(kSubtreeOutOfOrder) - 1);
    Respond(headers, kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);

    wchar_t path[] = L"/a b/m\u00e9dia";
    UNICODE_STRING pathString = MakePath(path);

    EXPECT_EQ(STATUS_PENDING, BlorgHttpGetDirectoryInfo(&pathString, 64, OnDirInfo, nullptr));

    Drain();

    ASSERT_EQ(1, LastDirInfo.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastDirInfo.Status);

    SIZE_T sentLength = 0;
    const char* text = (const char*)SandboxLastRequest(&sentLength);
    ASSERT_GT(sentLength, 0u);
    EXPECT_NE(nullptr, strstr(text, "GET /get_dir_info?path=%2Fa%20b%2Fm%C3%A9dia&subtree=64 HTTP/1.1\r\n"));
}

//
// A path whose encoding fits a request but leaves no room for the subtree
// query is refused before anything is sent: 21,840 spaces encode to
// 65,520 bytes, and the query's room would take the buffer past 64 KB.
//
TEST_F(HttpClientSubtreeTest, APathWithNoRoomForTheSubtreeQueryIsRefused)
{
    std::wstring path(21840, L' ');
    path[0] = L'/';
    UNICODE_STRING pathString = MakePath(&path[0]);

    EXPECT_EQ(STATUS_NAME_TOO_LONG, BlorgHttpGetDirectoryInfo(&pathString, 64, OnDirInfo, nullptr));
    EXPECT_EQ(0, LastDirInfo.Calls);
}

///////////////////////////////////////////////////////////////////////////
// Resource exhaustion
///////////////////////////////////////////////////////////////////////////

//
// Allocation failure at each position in turn. Every run must deliver
// exactly one outcome -- either the issue fails synchronously and the
// caller owns the error, or it returns pending and the callback fires
// once -- and must leave nothing behind. Both/neither would be a lost or
// double-completed IRP in the driver.
//
class HttpClientAllocationFailureTest : public HttpClientTest,
                                        public ::testing::WithParamInterface<LONG>
{
};

TEST_P(HttpClientAllocationFailureTest, DeliversExactlyOneOutcomeAndLeaksNothing)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char target[8] = {};

    ShimPoolFailAt(GetParam());

    NTSTATUS status = Read(target, sizeof(target));

    ShimPoolFailAt(-1);

    Drain();

    const int expected = (STATUS_PENDING == status) ? 1 : 0;

    EXPECT_EQ(expected, LastRead.Calls)
        << "an issue that returned 0x" << std::hex << status
        << " delivered " << std::dec << LastRead.Calls << " callback(s)";

    FreeMdl();
}

INSTANTIATE_TEST_SUITE_P(
    EveryAllocationSite,
    HttpClientAllocationFailureTest,
    ::testing::Range<LONG>(0, 12));

} // namespace
