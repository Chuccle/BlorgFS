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

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "SandboxSocket.h"
}

namespace
{

struct ReadResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Bytes = 0;
};

ReadResult LastRead;

void OnFileRead(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    (void)CallerContext;

    LastRead.Calls++;
    LastRead.Status = Status;
    LastRead.Bytes = FileBuffer ? FileBuffer->BodyBufferSize : 0;
}

struct FileInfoResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
};

FileInfoResult LastFileInfo;

void OnFileInfo(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* FileInfo, PVOID CallerContext)
{
    (void)FileInfo;
    (void)CallerContext;

    LastFileInfo.Calls++;
    LastFileInfo.Status = Status;
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

        EXPECT_EQ(0u, ShimPoolOutstanding()) << "pool allocation(s) leaked";
    }

    void Drain()
    {
        SandboxDrainCompletions();
        ShimDrainWorkItems();
    }

    // Issues a ranged read against the current script.
    NTSTATUS Read(unsigned char* target, SIZE_T length, SIZE_T offset = 0)
    {
        Mdl = ShimCreateMdl(target, length);

        wchar_t path[] = L"/media/file.bin";
        UNICODE_STRING pathString = MakePath(path);

        return BlorgHttpGetFileMdl(&pathString, offset, length, Mdl, OnFileRead, nullptr);
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

    for (SIZE_T i = requested; i < sizeof(canaried); ++i)
    {
        ASSERT_EQ(0xAA, canaried[i])
            << "byte " << i << " past the caller's " << requested << "-byte buffer was "
               "overwritten -- a peer that over-sends its declared Content-Length can "
               "write past the MDL the caller supplied";
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
        CLOSE_STEP,
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
