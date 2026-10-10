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

//
// A decoded listing keeps each name once, after its entries and its name
// index, NUL-terminated because DirCtrl enumerates it so, and finds every
// one through the index whatever its case, files first. The root of
// kSubtreeOutOfOrder lists the file r.bin and the subdirectories a and b.
// Names laid over one another, a terminator left out, or an index that
// misses or answers for the wrong entry each fail one of these.
//
TEST_F(HttpClientSubtreeTest, ADecodedListingsNamesStandApartAndAreFoundThroughItsIndex)
{
    PDIRECTORY_INFO root = List(0, kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);
    ASSERT_NE(nullptr, root);
    ASSERT_EQ(1u, root->FileCount);
    ASSERT_EQ(2u, root->SubDirCount);

    const PUCHAR names = (PUCHAR)root + root->NamesOffset;
    const PUCHAR end = (PUCHAR)root + root->Bytes;

    const struct
    {
        PWCH Name;
        SIZE_T NameLength;
        const wchar_t* Expected;
    } entries[] = {
        { BlorgGetFileEntry(root, 0)->Name, BlorgGetFileEntry(root, 0)->NameLength, L"r.bin" },
        { BlorgGetSubDirEntry(root, 0)->Name, BlorgGetSubDirEntry(root, 0)->NameLength, L"a" },
        { BlorgGetSubDirEntry(root, 1)->Name, BlorgGetSubDirEntry(root, 1)->NameLength, L"b" },
    };

    for (const auto& entry : entries)
    {
        EXPECT_EQ(std::wstring(entry.Expected), std::wstring(entry.Name));
        EXPECT_EQ(wcslen(entry.Expected), entry.NameLength);
        EXPECT_GE((PUCHAR)entry.Name, names);
        EXPECT_LE((PUCHAR)(entry.Name + entry.NameLength + 1), end);
    }

    const struct
    {
        const wchar_t* Name;
        BOOLEAN Found;
        SIZE_T Entry;
    } lookups[] = {
        { L"r.bin", TRUE, 0 },
        { L"R.BIN", TRUE, 0 },
        { L"a", TRUE, 1 },
        { L"B", TRUE, 2 },
        { L"c", FALSE, 0 },
        { L"r.bi", FALSE, 0 },
    };

    for (const auto& lookup : lookups)
    {
        wchar_t buffer[16];
        wcscpy_s(buffer, lookup.Name);
        UNICODE_STRING name = MakePath(buffer);
        SIZE_T entry = ~SIZE_T(0);

        EXPECT_EQ(lookup.Found, BlorgFindDirectoryEntry(root, &name, &entry)) << lookup.Name;

        if (lookup.Found)
        {
            EXPECT_EQ(lookup.Entry, entry) << lookup.Name;
        }
    }
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

//
// A zero-copy read's header buffer is 2 KB, which holds any real 206's
// headers. Growing it a page ahead of every header receive reallocated it
// on every read before a byte had arrived, and only made room for body
// bytes the read then copied out again. It grows once headers fill it.
//
// The reference is the same read with 3 KB of headers, which must grow
// exactly once: a buffer grown ahead of its first receive takes those
// headers without growing again, so both reads cost the same, and a buffer
// that never grows could not parse them at all.
//
TEST_F(HttpClientTest, AZeroCopyReadGrowsItsHeaderBufferOnlyOnceHeadersFillIt)
{
    std::vector<unsigned char> body(64 * 1024);

    for (SIZE_T i = 0; i < body.size(); ++i)
    {
        body[i] = C_CAST(unsigned char, i * 7);
    }

    std::string padded = "HTTP/1.1 206 Partial Content\r\nX-Pad: ";
    padded.append(3000, 'p');
    padded += "\r\nContent-Length: 65536\r\n\r\n";

    const char* const headers[] =
    {
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 65536\r\n\r\n",
        padded.c_str()
    };

    LONG allocations[RTL_NUMBER_OF(headers)] = {};

    for (SIZE_T i = 0; i < RTL_NUMBER_OF(headers); ++i)
    {
        std::vector<unsigned char> target(body.size());

        Respond(headers[i], body.data(), body.size());
        LastRead = {};
        ShimPoolFailAt(-1);

        ASSERT_EQ(STATUS_PENDING, Read(target.data(), target.size()));

        Drain();

        allocations[i] = ShimPoolAllocations();

        EXPECT_EQ(STATUS_SUCCESS, LastRead.Status) << "headers " << i;
        EXPECT_EQ(body, target) << "headers " << i;

        FreeMdl();
        BlorgCleanupWskClient();
    }

    EXPECT_EQ(allocations[0] + 1, allocations[1])
        << "a read whose headers fit the buffer must not regrow it";
}

SIZE_T BufferedReadBlock;
std::string BufferedReadBody;

void OnBufferedFileRead(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    OnFileRead(Status, FileBuffer, CallerContext);

    if (NT_SUCCESS(Status) && FileBuffer->BaseAddress)
    {
        BufferedReadBlock = ShimPoolBlockSize(FileBuffer->BaseAddress);
        BufferedReadBody.assign(FileBuffer->BodyBuffer, FileBuffer->BodyBufferSize);

        BlorgFreeHttpFile(FileBuffer);
    }
}

//
// A buffered fetch lands in the client's own buffer, sized for its headers
// and the body the 206 must match exactly. It was floored at 256 KB too, so
// every small fetch the disk cache or a fair-share refetch issued held
// 256 KB of nonpaged pool while it was in flight: 8 MB at the 32-fetch
// limit for what needed a few hundred KB.
//
TEST_F(HttpClientTest, ABufferedFetchHoldsOnlyItsHeadersAndBody)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n\r\nABCDEFGH")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    wchar_t path[] = L"/media/file.bin";
    UNICODE_STRING pathString = MakePath(path);

    BufferedReadBlock = 0;
    BufferedReadBody.clear();

    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetFile(&pathString, 0, 8, OnBufferedFileRead, nullptr));

    Drain();

    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status);
    EXPECT_EQ("ABCDEFGH", BufferedReadBody);
    EXPECT_GE(8u + 4096u, BufferedReadBlock) << "a buffered fetch is floored past its own headers and body";
}

///////////////////////////////////////////////////////////////////////////
// The TLS record layer
///////////////////////////////////////////////////////////////////////////

//
// A plaintext alert record arriving where the reply should be ends the
// connection, and on a pooled connection before any response byte that is
// the idle-close race: a server closing a keep-alive connection it timed
// out. Every other framing failure on a reused connection is retried once
// on a fresh one; the alert failed the read outright. The first read is
// plaintext and leaves its connection pooled with the alert still to come;
// the second runs over TLS, the stub's handshake keying the record layer
// so the request can be sent, and the fresh connection the retry opens
// answers with an alert too, which pins the single retry.
//
TEST_F(HttpClientTest, AnAlertOnAPooledTlsConnectionIsRetriedOnceOnAFreshOne)
{
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsGlobalInit());

    static const SANDBOX_STEP warmup[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWARM"),
        DELIVER("\x15\x03\x03\x00\x02\x01\x00")
    };

    SandboxSetPeerScript(warmup, RTL_NUMBER_OF(warmup));

    unsigned char first[4] = {};
    Read(first, sizeof(first));
    Drain();

    ASSERT_EQ(STATUS_SUCCESS, LastRead.Status);
    ASSERT_EQ(1u, SandboxSocketsPooled());
    FreeMdl();

    static const SANDBOX_STEP script[] =
    {
        DELIVER("\x15\x03\x03\x00\x02\x01\x00")
    };

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    global.TlsEnabled = TRUE;

    LastRead = {};
    const ULONG createdBefore = SandboxSocketsCreated();
    const ULONG64 retriesBefore = ShimStatistics.KeepAliveRetries;

    unsigned char second[4] = {};
    Read(second, sizeof(second));
    Drain();

    EXPECT_EQ(createdBefore + 1, SandboxSocketsCreated()) << "an alert on a pooled connection was not retried on a fresh one";
    EXPECT_EQ(retriesBefore + 1, ShimStatistics.KeepAliveRetries);
    EXPECT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_CONNECTION_RESET, LastRead.Status);

    FreeMdl();
    BlorgCleanupWskClient();
    BlorgTlsGlobalCleanup();
}

const UCHAR kClientWriteKey[TLS_KEY_LEN] =
    { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
const UCHAR kClientWriteIv[TLS_IV_LEN] =
    { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b };
const UCHAR kServerWriteKey[TLS_KEY_LEN] =
    { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f };
const UCHAR kServerWriteIv[TLS_IV_LEN] =
    { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b };

//
// The client with TLS on and the handshake stub installing known traffic
// keys, so everything after the handshake is the real record layer: the
// request sealed by HttpEncryptRequestRecord and the response opened by
// HttpIssueTlsReceive. The server's side is played here with Tls.c's own
// AEAD -- records sealed under the server's key, the client's opened
// under the client's.
//
class HttpClientTlsTest : public HttpClientTest
{
protected:
    void SetUp() override
    {
        HttpClientTest::SetUp();

        SavedRecvCapacity = SocketTlsRecvCapacity;

        ASSERT_EQ(STATUS_SUCCESS, BlorgTlsGlobalInit());

        global.TlsEnabled = TRUE;
        SandboxResetHandshakes();
        SandboxSetTrafficKeys(kClientWriteKey, kClientWriteIv, kServerWriteKey, kServerWriteIv);
    }

    void TearDown() override
    {
        HttpClientTest::TearDown();

        SocketTlsRecvCapacity = SavedRecvCapacity;
        SandboxResetHandshakes();
        global.TlsEnabled = FALSE;

        BlorgTlsGlobalCleanup();
    }

    //
    // One application_data record as the server sends it: Content, then
    // InnerType, then Padding zero bytes, sealed at Seq.
    //
    static std::vector<unsigned char> SealRecord(ULONGLONG Seq, UCHAR InnerType, const std::string& Content, size_t Padding = 0)
    {
        std::vector<unsigned char> inner(Content.begin(), Content.end());
        inner.push_back(InnerType);
        inner.insert(inner.end(), Padding, 0);

        const ULONG innerLength = C_CAST(ULONG, inner.size());
        const ULONG recordLength = innerLength + TLS_TAG_LEN;

        std::vector<unsigned char> record(5 + recordLength);
        record[0] = 0x17;
        record[1] = 0x03;
        record[2] = 0x03;
        record[3] = C_CAST(unsigned char, recordLength >> 8);
        record[4] = C_CAST(unsigned char, recordLength & 0xFF);

        EXPECT_EQ(STATUS_SUCCESS, BlorgTlsAeadEncrypt(
            kServerWriteKey, kServerWriteIv, Seq,
            record.data(), 5,
            inner.data(), innerLength,
            record.data() + 5, record.data() + 5 + innerLength));

        return record;
    }

    //
    // Each piece becomes one Deliver step, so one bulk receive takes one
    // piece however the records fall across them.
    //
    void DeliverInPieces(const std::vector<std::vector<unsigned char>>& NewPieces)
    {
        Pieces = NewPieces;
        Steps.clear();

        for (const std::vector<unsigned char>& piece : Pieces)
        {
            Steps.push_back({ SandboxStepDeliver, piece.data(), piece.size(), STATUS_SUCCESS, TRUE });
        }

        SandboxSetPeerScript(Steps.data(), Steps.size());
    }

    void DeliverInPieces(const std::vector<std::vector<unsigned char>>& NewPieces, const SANDBOX_STEP& Last)
    {
        DeliverInPieces(NewPieces);
        Steps.push_back(Last);
        SandboxSetPeerScript(Steps.data(), Steps.size());
    }

    static std::vector<unsigned char> Join(const std::vector<std::vector<unsigned char>>& Records)
    {
        std::vector<unsigned char> joined;

        for (const std::vector<unsigned char>& record : Records)
        {
            joined.insert(joined.end(), record.begin(), record.end());
        }

        return joined;
    }

    //
    // Opens the Index-th record the client sent on its latest connection.
    // Each is sealed at its own place in the client's sequence, so record
    // Index opens only at sequence Index.
    //
    static ::testing::AssertionResult OpenClientRecord(size_t Index, std::string* Request, UCHAR* InnerType)
    {
        SIZE_T sentLength = 0;
        const unsigned char* sent = SandboxLastRequest(&sentLength);
        SIZE_T offset = 0;

        for (size_t i = 0;; ++i)
        {
            if (offset + 5 > sentLength)
            {
                return ::testing::AssertionFailure() << "the client sent only " << i << " record(s)";
            }

            const ULONG recordLength = (C_CAST(ULONG, sent[offset + 3]) << 8) | sent[offset + 4];

            if (0x17 != sent[offset] || recordLength < TLS_TAG_LEN + 1 || offset + 5 + recordLength > sentLength)
            {
                return ::testing::AssertionFailure() << "record " << i << " is not a whole application_data record";
            }

            if (i == Index)
            {
                const ULONG innerLength = recordLength - TLS_TAG_LEN;
                std::vector<unsigned char> inner(innerLength);

                NTSTATUS status = BlorgTlsAeadDecrypt(
                    kClientWriteKey, kClientWriteIv, Index,
                    sent + offset, 5,
                    sent + offset + 5, innerLength, sent + offset + 5 + innerLength,
                    inner.data());

                if (!NT_SUCCESS(status))
                {
                    return ::testing::AssertionFailure() << "record " << i << " does not open at sequence " << Index;
                }

                *InnerType = inner.back();
                Request->assign(C_CAST(const char*, inner.data()), innerLength - 1);
                return ::testing::AssertionSuccess();
            }

            offset += 5 + recordLength;
        }
    }

    NTSTATUS GetFileInformation()
    {
        wchar_t path[] = L"/media/file.bin";
        UNICODE_STRING pathString = MakePath(path);

        return BlorgHttpGetFileInformation(&pathString, OnFileInfo, nullptr);
    }

    static std::string FileInfoResponse()
    {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sizeof(kFileInfo) - 1) + "\r\n\r\n" +
            std::string(kFileInfo, sizeof(kFileInfo) - 1);
    }

    std::vector<std::vector<unsigned char>> Pieces;
    std::vector<SANDBOX_STEP> Steps;
    ULONG SavedRecvCapacity = 0;
};

//
// Two requests over one kept-alive connection. Each goes out as a single
// application_data record sealed under the client's write key, with the
// request whole inside and 0x17 as its inner type, and the second one a
// sequence number on from the first; each answer opens at the server's
// next sequence number.
//
TEST_F(HttpClientTlsTest, RequestsAreSealedInSequenceOnAKeptAliveConnection)
{
    DeliverInPieces({ SealRecord(0, 0x17, FileInfoResponse()), SealRecord(1, 0x17, FileInfoResponse()) });

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();
    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();

    ASSERT_EQ(2, LastFileInfo.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastFileInfo.Status);
    EXPECT_EQ(4096u, LastFileInfo.Meta.Size);
    EXPECT_EQ(1u, SandboxSocketsCreated()) << "the second request did not stay on the kept-alive connection";
    EXPECT_EQ(1u, SandboxHandshakesStarted());

    for (size_t i = 0; i < 2; ++i)
    {
        std::string request;
        UCHAR innerType = 0;

        ASSERT_TRUE(OpenClientRecord(i, &request, &innerType));
        EXPECT_EQ(0x17, innerType) << "request " << i;
        EXPECT_EQ(0u, request.find("GET /get_dir_entry_info?path=")) << request;
        EXPECT_NE(std::string::npos, request.find("\r\n\r\n")) << "request " << i << " is not whole";
    }
}

//
// Records do not line up with receives. Here the first receive ends three
// bytes into the first record's header, the second ends inside the second
// record, and the third carries the rest of it and two more whole records,
// all of which the drain loop opens without another receive. The body
// lands in the caller's buffer in order. The last record's inner content
// type is the one byte that overhangs the buffer, so it too goes through
// the scratch, and the canary after the buffer checks that byte was not
// written in place.
//
TEST_F(HttpClientTlsTest, RecordsSplitAcrossReceivesAndPackedTogetherAreReassembled)
{
    const std::vector<unsigned char> stream = Join({
        SealRecord(0, 0x17, "HTTP/1.1 206 Partial Content\r\nContent-Length: 12\r\n\r\n"),
        SealRecord(1, 0x17, "ABCD"),
        SealRecord(2, 0x17, "EFGH"),
        SealRecord(3, 0x17, "IJKL") });

    const size_t firstCut = 3;
    const size_t secondCut = stream.size() - 2 * (5 + 5 + TLS_TAG_LEN) - 10;

    DeliverInPieces({
        std::vector<unsigned char>(stream.begin(), stream.begin() + firstCut),
        std::vector<unsigned char>(stream.begin() + firstCut, stream.begin() + secondCut),
        std::vector<unsigned char>(stream.begin() + secondCut, stream.end()) });

    const ULONG64 decryptedBefore = ShimStatistics.TlsRecordsDecrypted;

    unsigned char target[12 + 16];
    memset(target, 0xEE, sizeof(target));

    ASSERT_EQ(STATUS_PENDING, Read(target, 12));
    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status);
    EXPECT_EQ(12u, LastRead.Bytes);
    EXPECT_EQ(0, memcmp(target, "ABCDEFGHIJKL", 12));
    EXPECT_EQ(decryptedBefore + 4, ShimStatistics.TlsRecordsDecrypted);

    for (size_t i = 12; i < sizeof(target); ++i)
    {
        ASSERT_EQ(0xEE, target[i]) << "byte " << i << " past the caller's buffer was written";
    }

    FreeMdl();
}

//
// The record that ends the body carries its inner content type and any
// padding past the end of the caller's buffer, so it is opened into the
// socket's scratch and only its content copied out. Nothing past the
// buffer may be written, which the canary after it checks.
//
TEST_F(HttpClientTlsTest, FinalRecordOverhangingTheBufferIsCopiedFromScratch)
{
    DeliverInPieces({ Join({
        SealRecord(0, 0x17, "HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\n"),
        SealRecord(1, 0x17, "WXYZ", 64) }) });

    unsigned char target[4 + 80];
    memset(target, 0xEE, sizeof(target));

    ASSERT_EQ(STATUS_PENDING, Read(target, 4));
    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status);
    EXPECT_EQ(0, memcmp(target, "WXYZ", 4));

    for (size_t i = 4; i < sizeof(target); ++i)
    {
        ASSERT_EQ(0xEE, target[i]) << "byte " << i << " past the caller's buffer was written";
    }

    FreeMdl();
}

//
// A final record whose real content is longer than the room left is a
// server sending more than the Content-Length it declared. The read fails
// and nothing past the buffer is written.
//
TEST_F(HttpClientTlsTest, FinalRecordWithMoreContentThanRoomFailsTheRead)
{
    DeliverInPieces({ Join({
        SealRecord(0, 0x17, "HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\n"),
        SealRecord(1, 0x17, "WXYZWXYZ") }) });

    unsigned char target[4 + 80];
    memset(target, 0xEE, sizeof(target));

    ASSERT_EQ(STATUS_PENDING, Read(target, 4));
    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));

    for (size_t i = 4; i < sizeof(target); ++i)
    {
        ASSERT_EQ(0xEE, target[i]) << "byte " << i << " past the caller's buffer was written";
    }

    FreeMdl();
}

//
// A server sends NewSessionTicket after the handshake, as an encrypted
// record whose inner type is 0x16. There is no resumption cache to keep it
// in, so it is opened and dropped, and the response behind it is read as if
// it were not there.
//
TEST_F(HttpClientTlsTest, PostHandshakeMessageIsDiscarded)
{
    DeliverInPieces({ Join({
        SealRecord(0, 0x16, std::string(40, '\x04')),
        SealRecord(1, 0x17, FileInfoResponse()) }) });

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();

    ASSERT_EQ(1, LastFileInfo.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastFileInfo.Status);
    EXPECT_EQ(4096u, LastFileInfo.Meta.Size);
    EXPECT_EQ(1u, SandboxSocketsCreated()) << "the ticket was taken for a broken connection";
}

//
// A kept-alive connection the server closed with no close_notify: the next
// request on the pooled socket sees the connection end before any of its
// answer. That is the idle-close race, so it is retried once on a fresh
// connection, which handshakes again and restarts both sequences at zero.
//
TEST_F(HttpClientTlsTest, PooledConnectionThePeerClosedIsRetriedWithAFreshHandshake)
{
    DeliverInPieces({ SealRecord(0, 0x17, FileInfoResponse()) }, CLOSE_STEP);

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();
    ASSERT_EQ(1u, SandboxSocketsPooled());

    const ULONG64 retriesBefore = ShimStatistics.KeepAliveRetries;

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();

    ASSERT_EQ(2, LastFileInfo.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastFileInfo.Status);
    EXPECT_EQ(4096u, LastFileInfo.Meta.Size);
    EXPECT_EQ(retriesBefore + 1, ShimStatistics.KeepAliveRetries);
    EXPECT_EQ(2u, SandboxSocketsCreated());
    EXPECT_EQ(2u, SandboxHandshakesStarted());

    std::string request;
    UCHAR innerType = 0;
    EXPECT_TRUE(OpenClientRecord(0, &request, &innerType))
        << "the retried request must be sealed from the fresh connection's first sequence number";
}

//
// A server that idle-closes a kept-alive connection the way RFC 8446 6.1
// asks sends close_notify first: an alert, so outer type 0x17 with inner
// type 0x15 once opened. The next request on the pooled socket opens that
// before any of its answer, which is the same race as a bare close and is
// retried the same way. Taking the alert's two bytes as response, or
// failing outright, would fail a read the server was willing to answer.
//
TEST_F(HttpClientTlsTest, PooledConnectionClosedWithCloseNotifyIsRetriedOnAFreshOne)
{
    DeliverInPieces({ SealRecord(0, 0x17, FileInfoResponse()), SealRecord(1, 0x15, std::string("\x01\x00", 2)) },
        CLOSE_STEP);

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();
    ASSERT_EQ(1u, SandboxSocketsPooled());

    const ULONG64 retriesBefore = ShimStatistics.KeepAliveRetries;

    ASSERT_EQ(STATUS_PENDING, GetFileInformation());
    Drain();

    ASSERT_EQ(2, LastFileInfo.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastFileInfo.Status);
    EXPECT_EQ(4096u, LastFileInfo.Meta.Size);
    EXPECT_EQ(retriesBefore + 1, ShimStatistics.KeepAliveRetries);
    EXPECT_EQ(2u, SandboxSocketsCreated());
    EXPECT_EQ(2u, SandboxHandshakesStarted());
}

//
// Once the accumulator's free tail can no longer take a whole maximum-size
// record, the partial record left in it is moved to the front before the
// next receive. The accumulator here is one maximum-size record plus 2000
// bytes, and the first receive brings a 3000-byte record and the first
// 1000 bytes of a maximum-size one: the 15406 bytes still to come fit only
// once the 1000 have been moved down and the cursors reset. Without the
// move the receive is offered too little room and the read never
// finishes; a move that got the length or the cursors wrong breaks the
// record's tag.
//
TEST_F(HttpClientTlsTest, PartialRecordIsMovedToTheFrontWhenTheTailCannotTakeAWholeRecord)
{
    const ULONG maxRecord = 5 + TLS_RECORD_CIPHERTEXT_MAX;

    SocketTlsRecvCapacity = maxRecord + 2000;

    const std::string first(3000, 'a');
    const std::string second(16384, 'b');

    const std::vector<unsigned char> headers = SealRecord(0, 0x17,
        "HTTP/1.1 206 Partial Content\r\nContent-Length: " + std::to_string(first.size() + second.size()) + "\r\n\r\n");
    const std::vector<unsigned char> firstRecord = SealRecord(1, 0x17, first);
    const std::vector<unsigned char> secondRecord = SealRecord(2, 0x17, second);

    ASSERT_EQ(maxRecord, secondRecord.size());

    std::vector<unsigned char> arrival = Join({ headers, firstRecord });
    arrival.insert(arrival.end(), secondRecord.begin(), secondRecord.begin() + 1000);

    DeliverInPieces({ arrival, std::vector<unsigned char>(secondRecord.begin() + 1000, secondRecord.end()) });

    std::vector<unsigned char> target(first.size() + second.size());

    ASSERT_EQ(STATUS_PENDING, Read(target.data(), target.size()));
    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status);
    EXPECT_EQ(target.size(), LastRead.Bytes);
    EXPECT_EQ(0, memcmp(target.data(), (first + second).data(), target.size()));

    FreeMdl();
}

//
// Over TLS the record that ends a buffered body decrypts its inner
// content-type byte after the body's last byte, before the record layer
// strips it. A buffer pre-grown to end exactly at the body was reallocated
// for that byte, and the whole body copied again: every TLS listing,
// subtree and feed answer longer than its first buffer paid for it.
//
// The reference is the same answer short enough to land in the first
// buffer, which grows nothing; the long one, 2 KB records of a 20 KB body,
// grows once, for its body, when its headers are parsed -- whether its body
// starts on the 8-byte boundary flatcc wants or has to slide to one, since
// the two grow it in different places. A file-information answer is the
// buffered metadata the sandbox can encode, so the long one is the short
// one padded, which the verifier ignores.
//
TEST_F(HttpClientTlsTest, ABufferedBodyIsGrownOnceAndNotAgainForItsLastRecordsTypeByte)
{
    auto fetch = [this](const std::string& Body, SIZE_T Misalignment) -> LONG
    {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(Body.size()) + "\r\nX-Pad: ";

        while ((response.size() + 4) % 8 != Misalignment)
        {
            response += 'p';
        }

        response += "\r\n\r\n" + Body;

        std::vector<std::vector<unsigned char>> records;

        for (size_t at = 0; at < response.size(); at += 2048)
        {
            records.push_back(SealRecord(records.size(), 0x17, response.substr(at, 2048)));
        }

        DeliverInPieces({ Join(records) });

        wchar_t path[] = L"/media/file.bin";
        UNICODE_STRING pathString = MakePath(path);

        LastFileInfo = {};
        ShimPoolFailAt(-1);

        EXPECT_EQ(STATUS_PENDING, BlorgHttpGetFileInformation(&pathString, OnFileInfo, nullptr));

        Drain();

        LONG allocations = ShimPoolAllocations();

        EXPECT_EQ(1, LastFileInfo.Calls) << Body.size() << " bytes, misaligned by " << Misalignment;
        EXPECT_EQ(STATUS_SUCCESS, LastFileInfo.Status) << Body.size() << " bytes, misaligned by " << Misalignment;
        EXPECT_EQ(4096u, LastFileInfo.Meta.Size) << Body.size() << " bytes, misaligned by " << Misalignment;

        BlorgCleanupWskClient();

        return allocations;
    };

    const std::string shortBody(kFileInfo, sizeof(kFileInfo) - 1);
    std::string longBody = shortBody;
    longBody.append(20 * 1024, '\0');

    const LONG reference = fetch(shortBody, 4);

    for (SIZE_T misalignment : { SIZE_T(0), SIZE_T(4) })
    {
        EXPECT_EQ(reference + 1, fetch(longBody, misalignment))
            << "misaligned by " << misalignment
            << ": the body's buffer must be grown once, with room for the last record's type byte";
    }
}

} // namespace
