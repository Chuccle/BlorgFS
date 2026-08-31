//
// Async HTTP client used to talk to the Blorg metadata/file server.
// Every public entry point (BlorgHttpGetDirectoryInfo, BlorgHttpGetFileInfo,
// BlorgHttpGetFile/BlorgHttpGetFileMdl) issues a request and returns
// STATUS_PENDING; the result is delivered via a caller-supplied completion
// callback once the request/response cycle finishes on the WSK/IRP
// completion chain. Covers connection acquisition, optional TLS 1.3
// record-layer send/receive, HTTP header/body parsing, and FlatBuffer
// deserialization of directory/file metadata responses.
//

#include "Driver.h"
#include "Socket.h"
#include "TlsHandshake.h"

#define FLATCC_NO_ASSERT

#pragma warning(push)
#pragma warning(disable: 28110) // We don't use floating point numbers in our schema so no need to mess about with saving CPU state.
#include "generated/metadata_flatbuffer_reader.h"
#include "generated/metadata_flatbuffer_verifier.h"
#pragma warning(pop)

#include "picohttpparser.h"

#define HTTP_TAG 'PTTH'
//
// How many response headers picohttpparser is given room to report. This is
// not a policy limit -- it is the size of an array, and overflowing it is a
// hard parse failure (-1), not a truncation, so the whole response is
// rejected. At 16 that was reachable by ordinary servers: a plain nginx
// 206 already spends five or six on Date/Server/Content-Type/
// Content-Length/Connection/Accept-Ranges, and anything sitting behind a
// CDN or adding the usual security and CORS headers passes 16 without
// trying. The failure looked like a broken backend rather than a limit
// being hit, because nothing distinguishes this -1 from a malformed status
// line.
//
// 64 entries is 2 KB of the HTTP_CONTEXT, per in-flight request, which
// buys enough headroom that the array stops being a thing responses can
// collide with. It bounds nothing security-relevant on its own -- the byte
// ceiling does that -- so there is no reason to keep it tight.
//
#define HTTP_MAX_HEADERS 64
#define HTTP_INITIAL_RECV_CAPACITY (PAGE_SIZE * 4)
#define HTTP_FILE_INITIAL_RECV_CAPACITY (PAGE_SIZE * 64) // 256 KB initial capacity for file-read responses

//
// Initial receive capacity for a zero-copy (MDL) file read. Nothing but
// the response headers ever lands in Ctx->Buffer on that path -- the body
// goes straight to the caller's MDL -- so the buffer only has to hold a
// 206's status line and headers, a few hundred bytes in practice, with
// HttpGrowBufferIfNeeded growing a page at a time in the (unseen) case
// that a server sends more. Sizing it at the general
// HTTP_INITIAL_RECV_CAPACITY instead cost two things per chunk read: the
// larger NonPagedPoolNx allocation, and a larger header-phase receive,
// whose whole posted length gets filled with body bytes that
// HttpReadResponse then has to memcpy into the MDL (the spill copy) --
// bytes that at this size land in the MDL directly instead.
//
#define HTTP_MDL_INITIAL_RECV_CAPACITY 2048

//
// The TLS bulk-receive accumulator this file drains lives on the KSOCKET
// and is sized and allocated by Socket.c (SocketTlsRecvCapacity,
// BlorgEnsureTlsRecvBuffer). It is shared with the handshake, which fills it
// first: bytes the handshake's last bulk receive pulled in past the
// server's Finished -- a NewSessionTicket, typically -- stay buffered on
// the connection and are drained by the first request to use it, rather
// than being discarded and desyncing the record sequence.
//

//
// Hard ceiling on a single response body's declared Content-Length. The
// server is untrusted with respect to this value: without a ceiling, a
// malicious or buggy peer can force arbitrarily large NonPagedPoolNx
// allocations (see HttpAllocateContext) or drive integer overflow in the
// BodyOffset + ContentLength arithmetic used to size buffers and bound
// receive loops (see HttpCheckedAddSizeT). 64 MB covers a large directory
// listing and sizeable ranged file reads.
//
#define HTTP_MAX_CONTENT_LENGTH C_CAST(SIZE_T, (64 * 1024 * 1024))

//
// Hard ceiling on the status line plus headers of a single response, and
// the second half of the same untrusted-peer policy as
// HTTP_MAX_CONTENT_LENGTH. Content-Length bounds what a peer can make this
// driver allocate once it has declared a size; this bounds what it can
// make it allocate by never declaring one at all.
//
// The header phase completes on whatever arrives and re-posts until
// picohttpparser finds the terminating CRLFCRLF, growing Ctx->Buffer a page
// at a time as it goes -- so a peer that streams header bytes and simply
// never terminates them drove the buffer up to HttpGrowBufferIfNeeded's
// only limit, MAXULONG. That is close to 4 GB of NonPagedPoolNx per
// in-flight request, from a peer that has sent no valid response at all;
// non-paged pool exhaustion takes the machine down, not just this driver.
//
// 64 KB is several times what any real server sends (nginx defaults to
// 8 KB, IIS to 16 KB) and still leaves room for a listing response's
// headers to grow, so the ceiling is unreachable by accident.
//
#define HTTP_MAX_HEADER_BYTES C_CAST(ULONG, (64 * 1024))

//
// Checked SIZE_T addition. Returns FALSE (and leaves *Result unspecified)
// on overflow instead of wrapping. Every BodyOffset + ContentLength
// computation in this file -- combining a wire-parsed, untrusted length
// with another value -- must go through this: a wrapped sum can make a
// too-small buffer look big enough to a naive size check, turning
// overflow into an out-of-bounds read/write primitive.
//
//
// In-flight request gate, and why it is a base-referenced count rather
// than a rundown reference.
//
// Driver unload has to outlive nothing: a request still on the wire will
// later run its completion chain and its work items out of this image, so
// unloading while one is outstanding is a use-after-unload. The obvious
// primitive, EX_RUNDOWN_REF, is unusable here -- ExReleaseRundownProtection
// is documented IRQL <= APC_LEVEL, and HttpFreeContext runs at
// DISPATCH_LEVEL on the WSK completion chain for every file read.
//
// So: a plain interlocked count that starts at 1. That standing reference
// is what unload releases, and it is what makes the count reach zero
// exactly once -- an acquire refuses to lift the count off zero, so once
// the drain has started no new request can slip in behind it. Both
// operations are interlocked and legal at any IRQL, and the event is only
// ever waited on by the single PASSIVE-level unload path.
//
// The other candidate primitive is IO_REMOVE_LOCK: IoReleaseRemoveLock is
// documented <= DISPATCH_LEVEL where the rundown release is not, acquire
// refuses after IoReleaseRemoveLockAndWait exactly as above, and it would
// delete this whole block. Kept out deliberately -- it buys I/O-manager
// machinery for what two LONGs express -- but it is the shape to grow into
// if a third async issuer ever needs to share one unload gate with this
// one (the pre-warm pump's gate in Socket.c is the second of that kind).
//
static volatile LONG HttpActiveRequests = 1;
static KEVENT HttpDrainEvent;

//
// Takes a reference for one request. Returns FALSE once the count has
// reached zero, which only happens after BlorgDrainHttpClient has released the
// standing reference -- so a request issued during unload is refused
// rather than racing the teardown.
//
static BOOLEAN HttpAcquireActive(VOID)
{
    LONG current = ReadNoFence(&HttpActiveRequests);

    while (0 != current)
    {
        LONG previous = InterlockedCompareExchange(&HttpActiveRequests, current + 1, current);

        if (previous == current)
        {
            return TRUE;
        }

        current = previous;
    }

    return FALSE;
}

//
// Drops one reference; whoever drops the last one signals the drain.
// Callable at <= DISPATCH_LEVEL: KeSetEvent with Wait = FALSE is legal
// there, and no other side of this touches anything paged.
//
static VOID HttpReleaseActive(VOID)
{
    if (0 == InterlockedDecrement(&HttpActiveRequests))
    {
        KeSetEvent(&HttpDrainEvent, IO_NO_INCREMENT, FALSE);
    }
}

static BOOLEAN HttpCheckedAddSizeT(SIZE_T A, SIZE_T B, PSIZE_T Result)
{
    SIZE_T sum = A + B;

    if (sum < A)
    {
        return FALSE;
    }

    *Result = sum;
    return TRUE;
}

//
// Smallest number of body bytes a listing entry can possibly occupy on the
// wire: one uoffset_t in the vector. A flatbuffers vector of tables is a
// length followed by 4-byte offsets, and nothing stops every one of those
// offsets pointing at the same minimal table, so this is the true floor
// rather than a typical figure.
//
#define HTTP_MIN_LISTING_ENTRY_WIRE_BYTES 4

//
// Rejects entry counts a body of this size could not honestly describe.
//
// The counts come from the wire. flatcc's verifier bounds them to the
// buffer, which stops them being nonsense, but it does not stop them being
// enormously amplified: each 4-byte vector offset expands to a
// DIRECTORY_FILE_METADATA, which carries an inline WCHAR Name[260] and so
// costs 560 bytes. At HTTP_MAX_CONTENT_LENGTH that is 16.7M entries
// becoming an 8.8 GB PagedPool request from a single 64 MB response -- a
// ~140x amplification, and a memory-pressure DoS a malicious or
// compromised backend gets for free. The allocation failing cleanly is not
// much comfort when the machine has spent itself trying.
//
// So the ceiling on ContentLength is not sufficient on its own: it bounds
// the input, not what the input is inflated into. This ties the counts back
// to the body that carried them.
//
static BOOLEAN HttpListingCountsAreCredible(SIZE_T SubdirCount, SIZE_T FilesCount, SIZE_T BodyLen)
{
    SIZE_T totalCount = 0;

    if (!HttpCheckedAddSizeT(SubdirCount, FilesCount, &totalCount))
    {
        return FALSE;
    }

    if (totalCount > (BodyLen / HTTP_MIN_LISTING_ENTRY_WIRE_BYTES))
    {
        return FALSE;
    }

    return TRUE;
}

//
// Stack-depth note: WSK completion routines may run synchronously on the
// issuing thread when the underlying I/O finishes immediately. Each
// stage's completion routine does at most one thing before returning --
// issue the next async op or finish the chain -- and never re-enters a
// prior stage's dispatch switch on the same frame, keeping each link to a
// small, constant number of frames.
//
// The one loop that still self-iterates (draining a multi-chunk response
// into a growing buffer) re-issues BlorgReceiveWskAsync from inside
// HttpOnReceive; when WSK completes synchronously per chunk (common
// against a fast/local server) this adds real stack frames with no I/O
// manager dispatch loop to unwind through. HttpIssueReceive checks
// IoGetRemainingStackSize() before each receive and, when low, uses
// KeExpandKernelStackAndCalloutEx (Wait = FALSE, callable up to
// DISPATCH_LEVEL) to grow the stack and continue on the same thread,
// failing the single request only if stack is low above DISPATCH_LEVEL
// or expansion itself fails. See the comment above HttpIssueReceive.
//

// Which server operation a request is for.
typedef enum _HTTP_OPERATION
{
    HttpOpDirInfo,  // GET directory listing metadata
    HttpOpFileInfo, // GET single file/dir entry metadata
    HttpOpFileRead  // ranged GET of file content
} HTTP_OPERATION;

// Current step of the async request/response state machine.
typedef enum _HTTP_STAGE
{
    HttpStageAcquireSocket,
    HttpStageTlsHandshake, // only entered for a fresh socket without a completed handshake -- see HttpOnSocket
    HttpStageSendRequest,
    HttpStageReceive,
    HttpStageReadResponse,
    HttpStageDispatch,
    HttpStageComplete,
    HttpStageFailed
} HTTP_STAGE;

//
// Where the current connection came from; drives the keep-alive retry
// (see HttpTryRetryReusedConnection). One-way transitions, mutually
// exclusive states.
//
typedef enum _HTTP_CONNECTION_SOURCE
{
    //
    // Zero value (default for a zeroed context): acquire may hand back a
    // pooled keep-alive connection or a fresh one.
    //
    HttpConnectionPoolable,

    //
    // Socket reused from the keep-alive pool. A failure before any
    // response byte arrives is retryable (peer may have idle-closed it).
    // Only reached on the first attempt, so this state also implies
    // "not yet retried".
    //
    HttpConnectionReused,

    //
    // Guaranteed-fresh connect (pool was empty, or this is the
    // post-retry attempt). Acquire bypasses the pool; a failure here is
    // never retried again.
    //
    HttpConnectionFresh

} HTTP_CONNECTION_SOURCE;

typedef struct _HTTP_CONTEXT HTTP_CONTEXT;

//
// The PBLORG_*_COMPLETION callback signatures are declared in Client.h.
// Exactly one is invoked, exactly once, for a given HTTP_CONTEXT.
//

// Per-request state for one async HTTP request/response cycle.
typedef struct _HTTP_CONTEXT
{
    //
    // Parsed response headers; valid only between HttpParseHeaders
    // succeeding and HttpDispatch running (single-shot, not retained).
    //
    struct phr_header Headers[HTTP_MAX_HEADERS];

    // Server address for this request.
    SOCKADDR_STORAGE RemoteAddress;

    // Connection used for this request.
    PKSOCKET Socket;

    //
    // Request line + headers, built once. Not freed on send completion
    // (a reused-connection retry may resend it); freed in HttpFreeContext.
    //
    PCHAR RequestBuffer;
    ANSI_STRING EncodedPathBuffer;    // owns the URL-encoded path memory until request is built

    //
    // TLS-encrypted record wrapping RequestBuffer, sent instead of it
    // when Socket->Tls.State == TlsHandshakeComplete. Lazily allocated in
    // HttpEncryptRequestRecord, sized for one record (requests are always
    // small). TlsSendPlaintext is a separate staging buffer holding
    // RequestBuffer's bytes plus the trailing TLS inner content-type
    // byte; kept apart from TlsSendRecord because BlorgTlsAeadEncryptKeyed's
    // Plaintext/CiphertextOut parameters are restrict-qualified
    // (non-aliasing).
    //
    PCHAR TlsSendRecord;
    PCHAR TlsSendPlaintext;
    ULONG TlsSendRecordCapacity;
    ULONG TlsSendPlaintextCapacity;

    // Streaming receive buffer; grows via realloc-on-overflow.
    PCHAR Buffer;

    //
    // Body target for zero-copy file reads (BlorgHttpGetFileMdl): when
    // set, only headers land in Buffer and the body is received straight
    // into this already-locked MDL (body byte i at MDL offset
    // i == Length - BodyOffset). NULL means body is appended to Buffer.
    // Borrowed, never freed here; must stay locked until the completion
    // callback has run.
    //
    PMDL TargetMdl;

    //
    // Offset of the response body within Buffer (status line + headers +
    // terminating CRLF). 0 until HttpParseHeaders successfully parses the
    // headers, then set once -- doubles as "have headers been parsed yet?"
    //
    SIZE_T BodyOffset;
    SIZE_T ContentLength;

    //
    // Cached BodyOffset + ContentLength, computed and overflow-checked
    // once in HttpParseHeaders (see HttpCheckedAddSizeT). Valid only once
    // headers are parsed (BodyOffset != 0).
    //
    SIZE_T BodyEndOffset;

    SIZE_T HeaderCount;

    //
    // HttpOpFileRead only: exact byte count requested via the Range
    // header. A 206 response's Content-Length must equal this exactly,
    // checked independently of HTTP_MAX_CONTENT_LENGTH.
    //
    SIZE_T ExpectedContentLength;

    union
    {
        struct
        {
            PBLORG_DIRINFO_COMPLETION Routine;
        } DirInfo;

        struct
        {
            PBLORG_FILEINFO_COMPLETION Routine;
        } FileInfo;

        struct
        {
            PBLORG_FILEREAD_COMPLETION Routine;
        } FileRead;
    } Completion;

    PVOID CallerContext;

    //
    // Preallocated at context creation, freed in HttpFreeContext, so the
    // PASSIVE_LEVEL bounces (HttpDispatch/HttpComplete) can't fail on
    // allocation. At most one bounce is outstanding per context at a time.
    //
    PIO_WORKITEM WorkItem;

    HTTP_STAGE Stage;
    NTSTATUS FinalStatus;
    HTTP_CONNECTION_SOURCE ConnectionSource;
    HTTP_OPERATION Operation;
    int MinorVersion;
    int StatusCode;
    int ExpectedStatusCode; // 200 for dir/file-info, 206 for ranged file read
    ULONG RequestLength;
    ULONG Capacity;
    ULONG Length;

    //
    // QPC stamp taken when the request is built, so HttpComplete can fold
    // metadata requests into their latency counters (Statistics.h). File
    // reads are timed at their own issue sites instead -- the direct-fetch
    // path off the IRP's DriverContext -- because that wants the chunk
    // latency the reader
    // actually waits on, which starts before this context exists.
    //
    LONG64 IssueQpc;

    //
    // QPC stamp taken the moment the response headers finish parsing, i.e.
    // the driver's equivalent of time-to-first-byte. Recorded for file
    // reads specifically, because it is what separates "the server and
    // network are slow to answer" from "we are slow to take delivery" --
    // one number for issue-to-completion cannot tell those apart, and a
    // measured 5x gap between this driver and a plain usermode range GET
    // in the same guest is exactly the ambiguity that has to be resolved.
    //
    // Zero until the headers land, so a request that fails before that
    // contributes no TTFB sample rather than a bogus one.
    //
    LONG64 HeadersQpc;

    //
    // QPC stamp taken as the request send begins, so the pre-first-byte
    // time splits again: IssueQpc to here is everything before the request
    // is on the wire (socket acquisition from the pool, any connect, any
    // PASSIVE bounce to build the request), and here to HeadersQpc is the
    // part a usermode client's TTFB actually covers.
    //
    // Measured need: TTFB came out at 35 ms mean against 5.4 ms for the
    // same request from usermode in the same guest, on a pooled
    // connection. That 30 ms is either spent getting a socket and getting
    // to PASSIVE, or waiting on the peer, and those call for opposite
    // fixes.
    //
    LONG64 SendQpc;

    //
    // QPC stamp taken when the send completion fires, splitting the
    // pre-first-byte time once more: SendQpc to here is the request going
    // out and WSK telling us so, here to HeadersQpc is genuine wait on the
    // peer. A usermode client's TTFB is the second of those, so only the
    // second is a like-for-like comparison.
    //
    //
    // QPC stamp taken the moment WskSend has accepted the buffer and
    // returned STATUS_PENDING, splitting the send span once more.
    //
    // Real playback put the send at 5.1 ms mean and 164.7 ms max for a
    // request of about two hundred bytes, and made the worst
    // time-to-first-byte almost entirely send. Two very different things
    // were inside that one number: this driver building and submitting the
    // request, and the stack accepting it and delivering the completion.
    // The first would be an allocation or a lock; the second is TCP and DPC
    // scheduling under a saturated link, and they call for opposite fixes.
    //
    LONG64 SendIssuedQpc;

    LONG64 SendDoneQpc;

    //
    // QPC stamp taken once a socket is in hand, splitting the pre-send time
    // into "getting a connection" and "everything after". On a clean boot
    // pre-send showed an 18-29 ms mean with a 2.0 SECOND maximum, traced to
    // roughly three of only eleven fresh connects; on a warm pool it is
    // 0.3-1.2 ms. Those two shapes call for different fixes -- a connection
    // establishment problem versus a scheduling one -- and nothing so far
    // distinguishes them.
    //
    LONG64 SocketQpc;

} HTTP_CONTEXT;

static VOID HttpKick(HTTP_CONTEXT* Ctx);
static VOID HttpIssueReceive(HTTP_CONTEXT* Ctx);
static VOID HttpIssueReceiveDispatch(HTTP_CONTEXT* Ctx);
static BOOLEAN HttpTryRetryReusedConnection(HTTP_CONTEXT* Ctx);
static VOID HttpOnSocket(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID CompletionContext);
static VOID HttpOnTlsHandshakeComplete(NTSTATUS Status, PVOID CallerContext);
static VOID HttpTlsHandshakeWorker(PDEVICE_OBJECT DeviceObject, PVOID Context);
static VOID HttpOnSend(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext);
static VOID HttpOnReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext);
static VOID HttpIssueTlsReceive(HTTP_CONTEXT* Ctx);
static VOID HttpOnTlsReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext);

static NTSTATUS HttpParseHeaders(HTTP_CONTEXT* Ctx);
static NTSTATUS HttpGrowBufferIfNeeded(HTTP_CONTEXT* Ctx, SIZE_T RequiredCapacity);

static VOID HttpReadResponse(HTTP_CONTEXT* Ctx);
static VOID HttpDispatch(HTTP_CONTEXT* Ctx);
static VOID HttpComplete(HTTP_CONTEXT* Ctx, NTSTATUS Status);

static NTSTATUS HttpDeserializeDirectoryInfo(HTTP_CONTEXT* Ctx, PDIRECTORY_INFO* OutDirInfo);
static NTSTATUS HttpDeserializeDirectoryEntryInfo(HTTP_CONTEXT* Ctx, PDIRECTORY_ENTRY_METADATA DirEntryInfo);

///////////////////////////////////////////////////////////////////////////
// Small string/parsing helpers
///////////////////////////////////////////////////////////////////////////

static NTSTATUS StrToSize(const char* AsciiBuffer, SIZE_T Length, PSIZE_T Result)
{
    if (!AsciiBuffer || !Result || 0 == Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *Result = 0;
    SIZE_T i = 0;

    while (i < Length && AsciiBuffer[i] >= '0' && AsciiBuffer[i] <= '9')
    {
        if (*Result > (SIZE_MAX / 10) || (*Result == (SIZE_MAX / 10) && (AsciiBuffer[i] - '0') > (SIZE_MAX % 10)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        *Result = *Result * 10 + (AsciiBuffer[i] - '0');
        i++;
    }

    if (i < Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

//
// Case-insensitive ASCII header-name compare. Local by necessity, not
// preference: RtlEqualString(CaseInSensitive=TRUE) is documented
// PASSIVE_LEVEL only, and header parsing runs directly on the WSK
// completion chain (<= DISPATCH) for file reads. Header names are ASCII
// per RFC 9110, so a locale-free A-Z fold is exact.
//
static BOOLEAN HttpHeaderNameEquals(const char* Name, SIZE_T NameLength, const char* LowerCaseExpected, SIZE_T ExpectedLength)
{
    if (NameLength != ExpectedLength)
    {
        return FALSE;
    }

    for (SIZE_T i = 0; i < NameLength; ++i)
    {
        CHAR c = Name[i];

        if (c >= 'A' && c <= 'Z')
        {
            c += 'a' - 'A';
        }

        if (c != LowerCaseExpected[i])
        {
            return FALSE;
        }
    }

    return TRUE;
}

//
// Scans parsed headers for Content-Length and parses its value. Returns
// STATUS_NOT_FOUND if absent (caller decides whether that's an error), and
// STATUS_INVALID_NETWORK_RESPONSE if the response carries more than one.
//
// The whole header set is scanned rather than stopping at the first match,
// which is the point. Taking the first and ignoring the rest is the classic
// request-smuggling primitive read from the client side: this driver keeps
// a keep-alive connection pool, so if a proxy or origin ahead of it frames
// a response by a different Content-Length than the one used here, the
// leftover bytes stay in the stream and become the head of the NEXT
// response read on that socket -- one request's body served as another's.
// A response that declares its own length twice is malformed by RFC 9110
// either way; rejecting it costs nothing real and removes the ambiguity
// entirely, which is what "be conservative in what you accept" means for a
// length-prefixed protocol.
//
static NTSTATUS GetContentLengthFromHeaders(const struct phr_header* Headers, SIZE_T HeaderCount, PSIZE_T ContentLength)
{
    static const char contentLengthName[] = "content-length";

    NTSTATUS result = STATUS_NOT_FOUND;

    for (SIZE_T i = 0; i < HeaderCount; ++i)
    {
        if (!HttpHeaderNameEquals(Headers[i].name, Headers[i].name_len, contentLengthName, sizeof(contentLengthName) - 1))
        {
            continue;
        }

        if (STATUS_NOT_FOUND != result)
        {
            return STATUS_INVALID_NETWORK_RESPONSE;
        }

        result = StrToSize(Headers[i].value, Headers[i].value_len, ContentLength);
    }

    return result;
}

#define HEX_TO_CHAR(x) ((x) < 10 ? '0' + (x) : 'A' + (x) - 10)

//
// Tests whether a byte is an RFC 3986 unreserved character that can pass
// through URL-encoding unescaped.
//
static BOOLEAN IsCharacterSafeForUrl(UCHAR c)
{
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' ||
        c == '.' ||
        c == '_' ||
        c == '~')
    {
        return TRUE;
    }

    return FALSE;
}

//
// Percent-encodes a path into a NUL-terminated ANSI (in practice, UTF-8)
// string the request formatter can drop straight in.
//
// The output is ASCII by construction -- an unreserved byte passes through,
// anything else becomes '%' plus two hex digits -- which is why it does not
// go back through UTF-16. It used to build a UNICODE_STRING that
// HttpBuildRequest then handed to RtlStringCbPrintfA as %wZ, so every
// character was widened here and narrowed again there: twice the
// allocation, an extra conversion pass per request, and a %wZ on a path
// this driver otherwise takes trouble to keep clear of.
//
// Caller owns OutputString->Buffer on success and frees it with ExFreePool;
// on failure the buffer is freed here and nulled, so a failed call leaves
// nothing to clean up.
//
static NTSTATUS UrlEncodePathToAnsi(const UNICODE_STRING* InputString, PANSI_STRING OutputString)
{
    UTF8_STRING utf8String;

    NTSTATUS status = RtlUnicodeStringToUTF8String(&utf8String, InputString, TRUE);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PUCHAR utf8Buffer = C_CAST(PUCHAR, utf8String.Buffer);
    ULONG utf8Length = utf8String.Length;
    SIZE_T encodedLength = 0;

    for (ULONG i = 0; i < utf8Length; i++)
    {
        UCHAR c = utf8Buffer[i];
        encodedLength += IsCharacterSafeForUrl(c) ? 1 : 3;
    }

    if (encodedLength + 1 > MAXUSHORT)
    {
        RtlFreeUTF8String(&utf8String);
        return STATUS_NAME_TOO_LONG;
    }

    OutputString->Buffer = C_CAST(PCHAR, ExAllocatePoolUninitialized(
        NonPagedPoolNx,
        encodedLength + 1,
        'URLE'
    ));

    if (NULL == OutputString->Buffer)
    {
        RtlFreeUTF8String(&utf8String);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    OutputString->MaximumLength = C_CAST(USHORT, encodedLength + 1);

    ULONG j = 0;
    status = STATUS_SUCCESS;

    for (ULONG i = 0; i < utf8Length; i++)
    {
        UCHAR c = utf8Buffer[i];

        if (IsCharacterSafeForUrl(c))
        {
            if (j + 1 > C_CAST(ULONG, encodedLength))
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            OutputString->Buffer[j++] = C_CAST(CHAR, c);
        }
        else
        {
            if (j + 3 > C_CAST(ULONG, encodedLength))
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            OutputString->Buffer[j++] = '%';
            OutputString->Buffer[j++] = C_CAST(CHAR, HEX_TO_CHAR((c >> 4) & 0xF));
            OutputString->Buffer[j++] = C_CAST(CHAR, HEX_TO_CHAR(c & 0xF));
        }
    }

    OutputString->Buffer[j] = '\0';
    OutputString->Length = C_CAST(USHORT, j);

    RtlFreeUTF8String(&utf8String);

    if (!NT_SUCCESS(status))
    {
        ExFreePool(OutputString->Buffer);
        OutputString->Buffer = NULL;
    }

    return status;
}

///////////////////////////////////////////////////////////////////////////
// FlatBuffer deserialization
///////////////////////////////////////////////////////////////////////////

//
// Give flatcc's verifier an 8-byte-aligned view of the body. The streaming
// buffer base is pool-aligned, but BodyOffset can leave the body sub-region
// misaligned. Alignment is fixed in place, allocation-free: the body is
// slid down by the misalignment (at most 7 bytes) over the tail of the
// already-consumed headers -- Ctx->Headers' pointers into Buffer are
// single-shot and dead before dispatch runs (see the Headers field
// comment), and this is only called from the deserializers, after header
// parsing is long done. Cannot fail, so the callers carry no
// alignment-failure cleanup path.
//
static PCHAR HttpAlignBodyInPlace(PCHAR Body, SIZE_T BodyLen)
{
    SIZE_T misalignment = C_CAST(UINT_PTR, Body) & 0x7;

    if (misalignment)
    {
        RtlMoveMemory(Body - misalignment, Body, BodyLen);
    }

    return Body - misalignment;
}

//
// Verifies and decodes a FlatBuffer Directory response body into a newly
// allocated PDIRECTORY_INFO (header + inline file/subdir arrays). PASSIVE
// only (RtlUTF8ToUnicodeN), per the stage-machine gating above. The
// listing is PagedPool: every producer and consumer (this deserialize,
// the create path's serve-from-listing, directory enumeration, and the
// reap worker's free) runs at <= APC_LEVEL, and at 560 bytes per file
// entry a large flat directory would otherwise pin megabytes of
// non-paged pool for the DCB's whole lifetime.
// Server-supplied file and subdir names are untrusted: flatcc's verifier
// validates buffer structure but not that a decoded name fits the fixed
// Name[MAX_NAME_LEN] field, so each name is converted with
// RtlUTF8ToUnicodeN straight into the entry's Name -- no per-name
// intermediate allocation -- bounded to leave room for a NUL (the entry
// block is zero-allocated, so a bounded conversion stays terminated, and
// EnumerateDirectoryEntries reads Name as null-terminated). A name too
// long for the field fails its conversion outright and rejects the
// listing, the same policy the old explicit length check enforced.
//
static NTSTATUS HttpDeserializeDirectoryInfo(HTTP_CONTEXT* Ctx, PDIRECTORY_INFO* OutDirInfo)
{
    PCHAR body = Ctx->Buffer + Ctx->BodyOffset;
    SIZE_T bodyLen = Ctx->ContentLength;

    if (0 == bodyLen)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - empty body\n");
        return STATUS_INVALID_PARAMETER;
    }

    PCHAR alignedBuffer = HttpAlignBodyInPlace(body, bodyLen);

    int verifyCode = BlorgMetaFlat_Directory_verify_as_root(alignedBuffer, bodyLen);

    if (flatcc_verify_ok != verifyCode)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - %s\n", flatcc_verify_error_string(verifyCode));
        return STATUS_INVALID_PARAMETER;
    }

    BlorgMetaFlat_Directory_table_t directory = BlorgMetaFlat_Directory_as_root(alignedBuffer);

    if (!directory)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - directory knackered\n");
        return STATUS_INVALID_PARAMETER;
    }

    size_t headerSize = sizeof(DIRECTORY_INFO);

    BlorgMetaFlat_FileEntryMetadata_vec_t flatSubdirEntries = BlorgMetaFlat_Directory_subdirectories(directory);
    SIZE_T subdirCount = (flatSubdirEntries) ? BlorgMetaFlat_SubdirectoryMetadata_vec_len(flatSubdirEntries) : 0;

    BlorgMetaFlat_FileEntryMetadata_vec_t flatFileEntries = BlorgMetaFlat_Directory_files(directory);
    SIZE_T filesCount = (flatFileEntries) ? BlorgMetaFlat_FileEntryMetadata_vec_len(flatFileEntries) : 0;

    if (!HttpListingCountsAreCredible(subdirCount, filesCount, bodyLen))
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - implausible entry counts for a %Iu byte body\n", bodyLen);
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T filesEntryArraySize = filesCount * sizeof(DIRECTORY_FILE_METADATA);
    SIZE_T subDirArraySize = subdirCount * sizeof(DIRECTORY_SUBDIR_METADATA);

    SIZE_T entriesSize = 0;
    SIZE_T allocationSize = 0;

    if (!HttpCheckedAddSizeT(filesEntryArraySize, subDirArraySize, &entriesSize) ||
        !HttpCheckedAddSizeT(headerSize, entriesSize, &allocationSize))
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - listing size overflowed\n");
        return STATUS_INVALID_PARAMETER;
    }

    PDIRECTORY_INFO dirInfo = ExAllocatePoolZero(PagedPool, allocationSize, 'DBLR');

    if (!dirInfo)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - failed entries alloc\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dirInfo->FilesOffset = headerSize;
    dirInfo->SubDirsOffset = headerSize + filesEntryArraySize;
    dirInfo->FileCount = filesCount;
    dirInfo->SubDirCount = subdirCount;

    PDIRECTORY_FILE_METADATA fileEntries = BlorgGetFileEntry(dirInfo, 0);

    for (size_t i = 0; i < filesCount; ++i)
    {
        BlorgMetaFlat_FileEntryMetadata_table_t flatFileEntry = BlorgMetaFlat_FileEntryMetadata_vec_at(flatFileEntries, i);

        if (!flatFileEntry)
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - failed\n");
            ExFreePool(dirInfo);
            return STATUS_INVALID_PARAMETER;
        }

        flatbuffers_string_t name = BlorgMetaFlat_FileEntryMetadata_name(flatFileEntry);

        if (!name || flatbuffers_string_len(name) == 0)
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - failed\n");
            ExFreePool(dirInfo);
            return STATUS_INVALID_PARAMETER;
        }

        ULONG nameBytes = 0;
        NTSTATUS status = RtlUTF8ToUnicodeN(
            fileEntries[i].Name,
            (MAX_NAME_LEN - 1) * sizeof(WCHAR),
            &nameBytes,
            name,
            C_CAST(ULONG, flatbuffers_string_len(name)));

        if (!NT_SUCCESS(status))
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - file name conversion failed: %8lx\n", status);
            ExFreePool(dirInfo);
            return status;
        }

        fileEntries[i].NameLength = nameBytes / sizeof(WCHAR);

        fileEntries[i].Size = BlorgMetaFlat_FileEntryMetadata_size(flatFileEntry);
        fileEntries[i].CreationTime = BlorgMetaFlat_FileEntryMetadata_created(flatFileEntry);
        fileEntries[i].LastAccessedTime = BlorgMetaFlat_FileEntryMetadata_accessed(flatFileEntry);
        fileEntries[i].LastModifiedTime = BlorgMetaFlat_FileEntryMetadata_modified(flatFileEntry);
    }

    PDIRECTORY_SUBDIR_METADATA subdirEntries = BlorgGetSubDirEntry(dirInfo, 0);

    for (size_t i = 0; i < subdirCount; ++i)
    {
        BlorgMetaFlat_SubdirectoryMetadata_table_t flatSubdirEntry = BlorgMetaFlat_SubdirectoryMetadata_vec_at(flatSubdirEntries, i);

        if (!flatSubdirEntry)
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - failed\n");
            ExFreePool(dirInfo);
            return STATUS_INVALID_PARAMETER;
        }

        flatbuffers_string_t name = BlorgMetaFlat_SubdirectoryMetadata_name(flatSubdirEntry);

        if (!name || flatbuffers_string_len(name) == 0)
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - failed\n");
            ExFreePool(dirInfo);
            return STATUS_INVALID_PARAMETER;
        }

        ULONG nameBytes = 0;
        NTSTATUS status = RtlUTF8ToUnicodeN(
            subdirEntries[i].Name,
            (MAX_NAME_LEN - 1) * sizeof(WCHAR),
            &nameBytes,
            name,
            C_CAST(ULONG, flatbuffers_string_len(name)));

        if (!NT_SUCCESS(status))
        {
            BLORGFS_PRINT("HttpDeserializeDirectoryInfo() - subdir name conversion failed: %8lx\n", status);
            ExFreePool(dirInfo);
            return status;
        }

        subdirEntries[i].NameLength = nameBytes / sizeof(WCHAR);

        subdirEntries[i].CreationTime = BlorgMetaFlat_SubdirectoryMetadata_created(flatSubdirEntry);
        subdirEntries[i].LastAccessedTime = BlorgMetaFlat_SubdirectoryMetadata_accessed(flatSubdirEntry);
        subdirEntries[i].LastModifiedTime = BlorgMetaFlat_SubdirectoryMetadata_modified(flatSubdirEntry);
    }

    *OutDirInfo = dirInfo;
    return STATUS_SUCCESS;
}

//
// Verifies and decodes a FlatBuffer DirectoryEntryMetadata response body
// (single file/dir's stat info) directly into caller-provided DirEntryInfo.
//
static NTSTATUS HttpDeserializeDirectoryEntryInfo(HTTP_CONTEXT* Ctx, PDIRECTORY_ENTRY_METADATA DirEntryInfo)
{
    PCHAR body = Ctx->Buffer + Ctx->BodyOffset;
    SIZE_T bodyLen = Ctx->ContentLength;

    if (0 == bodyLen)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryEntryInfo() - empty body\n");
        return STATUS_INVALID_PARAMETER;
    }

    PCHAR alignedBuffer = HttpAlignBodyInPlace(body, bodyLen);

    int verifyCode = BlorgMetaFlat_DirectoryEntryMetadata_verify_as_root(alignedBuffer, bodyLen);

    if (flatcc_verify_ok != verifyCode)
    {
        BLORGFS_PRINT("HttpDeserializeDirectoryEntryInfo() - %s\n", flatcc_verify_error_string(verifyCode));
        return STATUS_INVALID_PARAMETER;
    }

    BlorgMetaFlat_DirectoryEntryMetadata_table_t dirEntMeta = BlorgMetaFlat_DirectoryEntryMetadata_as_root(alignedBuffer);

    if (!dirEntMeta)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DirEntryInfo->Size = BlorgMetaFlat_DirectoryEntryMetadata_size(dirEntMeta);
    DirEntryInfo->CreationTime = BlorgMetaFlat_DirectoryEntryMetadata_created(dirEntMeta);
    DirEntryInfo->LastAccessedTime = BlorgMetaFlat_DirectoryEntryMetadata_accessed(dirEntMeta);
    DirEntryInfo->LastModifiedTime = BlorgMetaFlat_DirectoryEntryMetadata_modified(dirEntMeta);
    DirEntryInfo->IsDirectory = BlorgMetaFlat_DirectoryEntryMetadata_directory(dirEntMeta);

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Context lifecycle
///////////////////////////////////////////////////////////////////////////

//
// Releases every resource owned by Ctx and frees Ctx itself. Ctx->Buffer is
// skipped only for a successful buffer-mode HttpOpFileRead, where ownership
// already transferred to the caller via HTTP_FILE_BUFFER.BaseAddress; every
// other case (zero-copy reads, where Buffer only ever held headers; the
// other two operations, which copy what they need out before HttpComplete
// runs; and any failure) still owns Buffer here.
//
static VOID HttpFreeContext(HTTP_CONTEXT* Ctx)
{
    if (Ctx->WorkItem)
    {
        IoFreeWorkItem(Ctx->WorkItem);
    }

    if (Ctx->RequestBuffer)
    {
        ExFreePool(Ctx->RequestBuffer);
    }

    if (Ctx->EncodedPathBuffer.Buffer)
    {
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
    }

    BOOLEAN bufferOwnedByCaller =
        (Ctx->Operation == HttpOpFileRead) && NT_SUCCESS(Ctx->FinalStatus) && !Ctx->TargetMdl;

    if (Ctx->Buffer && !bufferOwnedByCaller)
    {
        ExFreePool(Ctx->Buffer);
    }

    if (Ctx->TlsSendRecord)
    {
        ExFreePool(Ctx->TlsSendRecord);
    }

    if (Ctx->TlsSendPlaintext)
    {
        ExFreePool(Ctx->TlsSendPlaintext);
    }

    ExFreePool(Ctx);

    HttpReleaseActive();
}

//
// Grows Ctx->Buffer in place (realloc) if RequiredCapacity exceeds the
// current capacity; no-op otherwise. RequiredCapacity is rejected above
// MAXULONG since Capacity is stored as ULONG. ReallocateBufferUninitialized
// returns the original buffer (untouched) on allocation failure and always
// yields a distinct pointer on success, so failure is detected by pointer
// equality with the prior buffer.
//
static NTSTATUS HttpGrowBufferIfNeeded(HTTP_CONTEXT* Ctx, SIZE_T RequiredCapacity)
{
    if (RequiredCapacity <= Ctx->Capacity)
    {
        return STATUS_SUCCESS;
    }

    if (RequiredCapacity > MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PCHAR newBuffer = ReallocateBufferUninitialized(
        Ctx->Buffer,
        Ctx->Length,
        NonPagedPoolNx,
        RequiredCapacity,
        HTTP_TAG);

    if (newBuffer == Ctx->Buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ctx->Buffer = newBuffer;
    Ctx->Capacity = C_CAST(ULONG, RequiredCapacity);

    return STATUS_SUCCESS;
}

//
// Wraps Ctx->RequestBuffer/RequestLength in a single TLS
// application-data record (Ctx->TlsSendRecord), using the write
// application traffic key TlsHandshakeSendClientFinished cached into
// Socket->Tls.WriteKeyHandle. One record is always enough: a request
// this driver builds (GET/HEAD line + headers) is always far under the
// 2^14-byte TLSInnerPlaintext limit -- there is no multi-record loop
// here the way there is on the receive side; the innerLen bound below is
// therefore unreachable in practice, but is kept as a hard policy check
// (consistent with HttpCheckedAddSizeT elsewhere) rather than assumed
// away. TlsSendPlaintext/TlsSendRecord are grown on demand rather than
// allocated once, since a retried request runs through this function
// again on a fresh connection and must not assume a previously-cached
// buffer is still big enough (same principle as HttpGrowBufferIfNeeded).
// The inner plaintext's trailing byte is the TLS inner content-type
// marker (0x17 == application_data), appended with no padding.
// recordAllocSize's own wraparound check (recordAllocSize < 5) is likewise
// unreachable in practice, since recordLen is already bounded well under
// that by the TLS_RECORD_CIPHERTEXT_MAX check above, but is kept
// self-contained right where the addition happens.
//
static NTSTATUS HttpEncryptRequestRecord(HTTP_CONTEXT* Ctx, PCHAR* SendBufferOut, ULONG* SendLengthOut)
{
    ULONG innerLen = Ctx->RequestLength + 1;
    ULONG recordLen = innerLen + TLS_TAG_LEN;

    if (innerLen > TLS_RECORD_CIPHERTEXT_MAX - TLS_TAG_LEN)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Ctx->TlsSendPlaintextCapacity < innerLen)
    {
        if (Ctx->TlsSendPlaintext)
        {
            ExFreePool(Ctx->TlsSendPlaintext);
        }

        Ctx->TlsSendPlaintext = ExAllocatePoolZero(NonPagedPoolNx, innerLen, HTTP_TAG);
        Ctx->TlsSendPlaintextCapacity = Ctx->TlsSendPlaintext ? innerLen : 0;

        if (!Ctx->TlsSendPlaintext)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ULONG recordAllocSize = 5 + recordLen;

    if (recordAllocSize < 5)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Ctx->TlsSendRecordCapacity < recordAllocSize)
    {
        if (Ctx->TlsSendRecord)
        {
            ExFreePool(Ctx->TlsSendRecord);
        }

        Ctx->TlsSendRecord = ExAllocatePoolZero(NonPagedPoolNx, recordAllocSize, HTTP_TAG);
        Ctx->TlsSendRecordCapacity = Ctx->TlsSendRecord ? recordAllocSize : 0;

        if (!Ctx->TlsSendRecord)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    RtlCopyMemory(Ctx->TlsSendPlaintext, Ctx->RequestBuffer, Ctx->RequestLength);
    Ctx->TlsSendPlaintext[Ctx->RequestLength] = 0x17;

    Ctx->TlsSendRecord[0] = 0x17;
    Ctx->TlsSendRecord[1] = 0x03;
    Ctx->TlsSendRecord[2] = 0x03;
    Ctx->TlsSendRecord[3] = C_CAST(UCHAR, recordLen >> 8);
    Ctx->TlsSendRecord[4] = C_CAST(UCHAR, recordLen & 0xFF);

    NTSTATUS status = BlorgTlsAeadEncryptKeyed(
        Ctx->Socket->Tls.WriteKeyHandle, Ctx->Socket->Tls.WriteIv, Ctx->Socket->Tls.WriteSeq,
        C_CAST(PUCHAR, Ctx->TlsSendRecord), 5,
        C_CAST(PUCHAR, Ctx->TlsSendPlaintext), innerLen,
        C_CAST(PUCHAR, Ctx->TlsSendRecord) + 5, C_CAST(PUCHAR, Ctx->TlsSendRecord) + 5 + innerLen);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    Ctx->Socket->Tls.WriteSeq++;

    *SendBufferOut = Ctx->TlsSendRecord;
    *SendLengthOut = 5 + recordLen;
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Stage machine
//
// Each Http* function below is a completion routine (or is only ever
// called from one). Each does at most one of: issue the next async op,
// or call HttpComplete. None of them call back into a *previous* stage
// on the same frame -- the chain only ever moves forward, so the I/O
// manager's own completion dispatch is what unwinds prior frames between
// stages, not anything this code does explicitly.
///////////////////////////////////////////////////////////////////////////

//
// The single IRQL rule for the back half of the stage machine: receive
// and parse (HttpReadResponse) are DISPATCH-safe for every operation and
// always run inline on the WSK completion chain; the two things that are
// NOT -- flatcc deserialization and the dir/file-info completion
// callbacks -- sit behind this gate at their exact entry points,
// HttpDispatch (success) and HttpComplete (failure/teardown).
//
//  - HttpOpFileRead never bounces: no deserialize step, and its
//    callbacks (ReadComplete) are contracted
//    DISPATCH-safe. This is the read hot path -- no work-item queue, no
//    context switch per chunk.
//
//  - The other operations' callbacks (DirCtrlComplete,
//    CreateComplete, BlorgFileInfoComplete) take push locks inside
//    KeEnterCriticalRegion (PathCache, DCB listing cache) -- APC_LEVEL-
//    or-below work -- and their bodies deserialize with flatcc (PASSIVE
//    only), so they pay exactly one PASSIVE hop per request. A
//    DISPATCH-level callback invocation here is a Driver Verifier 0xC4
//    (KeEnterCriticalRegion IRQL rule). Failure paths need the gate as
//    much as success paths: any HttpFail fired from a WSK completion
//    (socket error, timeout, protocol violation) arrives at DISPATCH.
//
// Note the callback IRQL contract in Client.h is per-operation because
// of this: "<= DISPATCH" for file reads, PASSIVE for the rest.
//
static BOOLEAN HttpMustBounceToPassive(const HTTP_CONTEXT* Ctx)
{
    return HttpOpFileRead != Ctx->Operation && KeGetCurrentIrql() > PASSIVE_LEVEL;
}

//
// Whether this operation can ever queue Ctx->WorkItem. Only two things
// do: the HttpDispatch/HttpComplete bounces (HttpMustBounceToPassive,
// never true for a file read) and HttpKick's TLS handshake stage. So a
// file read on a plaintext connection needs no work item at all, and
// skipping it takes one pool allocation and one free off every chunk on
// the read hot path. global.TlsEnabled is sampled here, at the one point
// where an allocation failure can still be reported to the caller, rather
// than at handshake time; HttpKick re-checks for NULL so that flipping
// the flag live (the debugger poke documented in Driver.h) degrades to a
// failed request rather than a NULL dereference.
//
static BOOLEAN HttpNeedsWorkItem(HTTP_OPERATION Operation)
{
    return HttpOpFileRead != Operation || global.TlsEnabled;
}

//
// Stage-machine dispatcher: drives Ctx forward one step per call by
// switching on Ctx->Stage and issuing the next async op (or completing).
// Central re-entry point for every stage transition in this file.
// HttpStageAcquireSocket bypasses the pool only on the post-retry attempt
// (the retry path forced ConnectionSource to Fresh); the initial attempt
// is free to reuse a pooled connection. HttpStageTlsHandshake bounces to
// PASSIVE unconditionally (unlike HttpMustBounceToPassive, independent of
// Ctx->Operation) because BlorgTlsStartHandshakeAsync's ECDH key-pair
// generation is documented PASSIVE_LEVEL-only CNG, while this stage can be
// entered from a WSK connect completion at <= DISPATCH_LEVEL. In
// HttpStageSendRequest, WSK_FLAG_NODELAY is used because the request is
// one small, complete send with nothing further going out until the
// response arrives, so Nagle could only ever hold its tail back waiting
// for an ACK that gains nothing.
//
static VOID HttpKick(HTTP_CONTEXT* Ctx)
{
    switch (Ctx->Stage)
    {
        case HttpStageAcquireSocket:
        {
            BOOLEAN forceFresh = C_CAST(BOOLEAN, Ctx->ConnectionSource == HttpConnectionFresh);

            NTSTATUS result = BlorgAcquireReusableWskSocketAsync(
                C_CAST(PSOCKADDR, &Ctx->RemoteAddress),
                forceFresh,
                HttpOnSocket,
                Ctx);

            if (!NT_SUCCESS(result) && STATUS_PENDING != result)
            {
                HttpComplete(Ctx, result);
            }
            break;
        }

        case HttpStageTlsHandshake:
        {
            if (KeGetCurrentIrql() > PASSIVE_LEVEL)
            {
                if (!Ctx->WorkItem)
                {
                    HttpComplete(Ctx, STATUS_INSUFFICIENT_RESOURCES);
                    break;
                }

                IoQueueWorkItem(Ctx->WorkItem, HttpTlsHandshakeWorker, DelayedWorkQueue, Ctx);
            }
            else
            {
                BlorgTlsStartHandshakeAsync(Ctx->Socket, HttpOnTlsHandshakeComplete, Ctx);
            }
            break;
        }

        case HttpStageSendRequest:
        {
            if (0 == Ctx->SendQpc)
            {
                Ctx->SendQpc = BlorgStatisticsNow();
            }

            PCHAR sendBuffer = Ctx->RequestBuffer;
            ULONG sendLength = Ctx->RequestLength;
            NTSTATUS result;

            if (TlsHandshakeComplete == Ctx->Socket->Tls.State)
            {
                result = HttpEncryptRequestRecord(Ctx, &sendBuffer, &sendLength);

                if (!NT_SUCCESS(result))
                {
                    HttpComplete(Ctx, result);
                    break;
                }
            }

            result = BlorgSendWskAsync(
                Ctx->Socket,
                sendBuffer,
                sendLength,
                WSK_FLAG_NODELAY,
                HttpOnSend,
                Ctx);

            Ctx->SendIssuedQpc = BlorgStatisticsNow();

            if (STATUS_PENDING != result)
            {
                HttpComplete(Ctx, result);
            }
            break;
        }

        case HttpStageReceive:
        {
            HttpIssueReceiveDispatch(Ctx);
            break;
        }

        case HttpStageReadResponse:
        {
            HttpReadResponse(Ctx);
            break;
        }

        case HttpStageDispatch:
        {
            HttpDispatch(Ctx);
            break;
        }

        case HttpStageComplete:
        {
            HttpComplete(Ctx, STATUS_SUCCESS);
            break;
        }

        case HttpStageFailed:
        {
            HttpComplete(Ctx, Ctx->FinalStatus);
            break;
        }

        default:
        {
            HttpComplete(Ctx, STATUS_INVALID_PARAMETER);
            break;
        }
    }
}

//
// Terminal failure transition. Routing through HttpStageFailed (rather
// than calling HttpComplete directly) keeps every failure on the same
// forward path as the success transitions. The socket, if still held,
// is closed by HttpComplete's failure path -- callers must not close it
// here.
//
static VOID HttpFail(HTTP_CONTEXT* Ctx, NTSTATUS Status)
{
    BLORGFS_LOG("HttpFail: operation=%d stage=%d source=%d status=0x%08lX\n",
        Ctx->Operation, Ctx->Stage, Ctx->ConnectionSource, Status);

    Ctx->Stage = HttpStageFailed;
    Ctx->FinalStatus = Status;
    HttpKick(Ctx);
}

//
// A send/receive failure on a pooled keep-alive socket, before any
// response byte has been seen, is the expected idle-close race: closes
// the dead socket and re-issues the (idempotent GET) request once on a
// guaranteed-fresh connection. Returns TRUE if a retry was launched (caller
// must not touch Ctx further); FALSE if not retryable (fresh connection,
// already retried, or response bytes already consumed). Per-attempt
// response-parse state is reset for a clean retry parse, but Buffer's
// Capacity is kept and RequestBuffer stays intact, since both are still
// valid and reusable on the fresh connection. The TLS bulk accumulator
// needs no reset here: it lives on the KSOCKET (Socket.h), so it is
// freed with the dead socket and the fresh connection starts with a
// zeroed one of its own.
//
static BOOLEAN HttpTryRetryReusedConnection(HTTP_CONTEXT* Ctx)
{
    if (HttpConnectionReused != Ctx->ConnectionSource || 0 != Ctx->Length)
    {
        return FALSE;
    }

    BLORGFS_PRINT("HttpTryRetryReusedConnection: reused connection failed pre-response, retrying fresh\n");

    BLORGFS_STAT_INC(KeepAliveRetries);

    if (Ctx->Socket)
    {
        BlorgCloseWskSocketAsync(Ctx->Socket);
        Ctx->Socket = NULL;
    }

    Ctx->ConnectionSource = HttpConnectionFresh;

    Ctx->Length = 0;
    Ctx->BodyOffset = 0;
    Ctx->ContentLength = 0;
    Ctx->BodyEndOffset = 0;
    Ctx->HeaderCount = 0;
    Ctx->StatusCode = 0;

    Ctx->Stage = HttpStageAcquireSocket;
    HttpKick(Ctx);
    return TRUE;
}

//
// Shared "connection died before Ctx's response was fully read" decision:
// retry once via HttpTryRetryReusedConnection, else fail with Status.
// Used by both HttpOnReceive (plaintext) and HttpOnTlsReceive (TLS
// record layer).
//
static VOID HttpFailOrRetryReusedConnection(HTTP_CONTEXT* Ctx, NTSTATUS Status)
{
    if (HttpTryRetryReusedConnection(Ctx))
    {
        return;
    }

    HttpFail(Ctx, Status);
}

//
// Completion for connection acquisition: on success records the socket and
// routes to the TLS handshake stage (if enabled and not already done on a
// pooled socket) or straight to sending the request. global.TlsEnabled
// gates the whole handshake path (see its declaration in Driver.h) --
// disabled by default, so this is a no-op until explicitly turned on. When
// enabled, a pooled socket that already completed a handshake (State ==
// TlsHandshakeComplete) skips straight to sending the request; anything
// else (always true for a fresh connection) runs the handshake first. A
// handshake failure is always a hard error here, never the idle-close
// retry case, since a reused socket by construction already has a
// completed handshake, so this stage is only ever reached for fresh
// connections. No separate pool-release guard is needed on that failure:
// HttpComplete's failure path already closes rather than pools any socket
// still held on failure, and the only success path that pools a socket
// (HttpReadResponse, after a full response is read) is unreachable unless
// everything before it -- including any handshake attempted on this
// socket -- already succeeded.
//
static VOID HttpOnSocket(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID CompletionContext)
{
    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, CompletionContext);

    if (!NT_SUCCESS(Status))
    {
        HttpFail(ctx, Status);
        return;
    }

    ctx->Socket = Socket;
    ctx->ConnectionSource = Reused ? HttpConnectionReused : HttpConnectionFresh;
    ctx->SocketQpc = BlorgStatisticsNow();

    if (global.TlsEnabled && TlsHandshakeComplete != Socket->Tls.State)
    {
        ctx->Stage = HttpStageTlsHandshake;
    }
    else
    {
        ctx->Stage = HttpStageSendRequest;
    }

    HttpKick(ctx);
}

//
// Re-entry runs at PASSIVE_LEVEL, so BlorgTlsStartHandshakeAsync's PASSIVE-
// only CNG calls (BCryptGenerateKeyPair/BCryptFinalizeKeyPair) are safe
// here regardless of what IRQL triggered the bounce in HttpKick.
//
static VOID HttpTlsHandshakeWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, Context);
    BlorgTlsStartHandshakeAsync(ctx->Socket, HttpOnTlsHandshakeComplete, ctx);
}

//
// Completion for BlorgTlsStartHandshakeAsync: advances to sending the request
// on success, otherwise fails the request (never the idle-close retry
// case -- see the comment in HttpOnSocket).
//
static VOID HttpOnTlsHandshakeComplete(NTSTATUS Status, PVOID CallerContext)
{
    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, CallerContext);

    if (!NT_SUCCESS(Status))
    {
        HttpFail(ctx, Status);
        return;
    }

    ctx->Stage = HttpStageSendRequest;
    HttpKick(ctx);
}

//
// Completion for the request send: on failure retries once if this was a
// pooled reused connection, otherwise advances to the receive stage.
// BytesTransferred is unused: WskSend's contract is "all or error" for
// stream sockets, so a successful completion means the whole request is on
// the wire. RequestBuffer and EncodedPathBuffer are deliberately NOT freed
// here even though the send is done: a reused connection can still turn
// out to be dead on the subsequent receive (the common idle-close case --
// the send is accepted into the local TCP buffer but the peer's FIN
// surfaces as a 0-byte receive), and the retry resends the same request.
// Both are freed unconditionally in HttpFreeContext.
//
static VOID HttpOnSend(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext)
{
    UNREFERENCED_PARAMETER(BytesTransferred);

    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, CompletionContext);

    if (!NT_SUCCESS(Status))
    {
        HttpFailOrRetryReusedConnection(ctx, Status);
        return;
    }

    ctx->SendDoneQpc = BlorgStatisticsNow();
    ctx->Stage = HttpStageReceive;
    HttpKick(ctx);
}

//
// HttpIssueReceive and the synchronous-chain bound
// ---------------------------------------------------------------------
//
// If the peer streams a response in N chunks and WSK completes each
// receive synchronously (same thread, inline, before WskReceive returns
// -- common against a fast/local server), each chunk adds real frames to
// the stack: HttpOnReceive -> HttpIssueReceive -> BlorgReceiveWskAsync -> WSK's
// internal dispatch -> WskReceive -> (inline completion) -> HttpOnReceive
// again. There is no I/O manager dispatch loop to unwind between chunks
// when completion is inline -- that loop only exists for genuinely
// asynchronous (DPC-deferred) completions.
//
// Rather than guess a fixed chain-length cap, IoGetRemainingStackSize()
// is checked before each synchronous-capable receive:
//
//   1. Plenty of stack remains (> HTTP_STACK_SAFETY_MARGIN) -- issue the
//      receive directly on the current stack.
//
//   2. Stack is low, but at an IRQL where KeExpandKernelStackAndCalloutEx
//      is legal (<= DISPATCH_LEVEL, Wait = FALSE) -- expand the stack and
//      re-enter HttpIssueReceive via the callout. Still synchronous,
//      same thread, zero waits, zero work-item scheduling. The Ex variant
//      (not plain KeExpandKernelStackAndCallout, <= APC_LEVEL only) is
//      what makes this legal from a WSK completion routine at
//      DISPATCH_LEVEL.
//
//   3. Stack is low AND above DISPATCH_LEVEL (not expected for a WSK
//      completion routine, kept as a defensive floor), or expansion
//      itself fails -- fail the single request with
//      STATUS_INSUFFICIENT_RESOURCES rather than risk overflow.
//
// HTTP_STACK_SAFETY_MARGIN covers one chain link's worst-case usage (this
// function, HttpOnReceive, BlorgReceiveWskAsync/WskReceive internals, the
// completion routine) plus headroom for WSK's own internal usage.
//
// HTTP_STACK_EXPAND_SIZE is handed to KeExpandKernelStackAndCalloutEx --
// large enough for many further chain links before expanding again,
// while staying well under MAXIMUM_EXPANSION_SIZE.
//

#define HTTP_STACK_SAFETY_MARGIN  (PAGE_SIZE * 2)   // ~2 chain links' worth, deliberately generous
#define HTTP_STACK_EXPAND_SIZE    (PAGE_SIZE * 8)   // headroom for many further links post-expansion

//
// KeExpandKernelStackAndCalloutEx callout target: re-enters HttpIssueReceive
// with more stack available. See the stack-safety discussion above. Runs
// with HTTP_STACK_EXPAND_SIZE additional bytes available on (effectively)
// the same logical call chain; the normal entry point will re-check
// IoGetRemainingStackSize() itself and proceed via case 1 for a good while
// before potentially needing to expand again.
//
static VOID HttpIssueReceiveExpandedCallout(PVOID Parameter)
{
    HttpIssueReceive(C_CAST(HTTP_CONTEXT*, Parameter));
}

//
// Single dispatch point for "issue whatever receive this connection
// needs next" -- plaintext (HttpIssueReceive) or TLS record-framed
// (HttpIssueTlsReceive), chosen by Socket->Tls.State. Used at every
// re-entry point (HttpKick's HttpStageReceive case, and the two places
// inside HttpReadResponse that re-issue directly without going through
// HttpKick) so none of them can drift out of sync with each other.
//
static VOID HttpIssueReceiveDispatch(HTTP_CONTEXT* Ctx)
{
    if (TlsHandshakeComplete == Ctx->Socket->Tls.State)
    {
        HttpIssueTlsReceive(Ctx);
    }
    else
    {
        HttpIssueReceive(Ctx);
    }
}

//
// Issues the next plaintext receive: header-phase (grows Buffer a page at
// a time, bounded by HTTP_MAX_HEADER_BYTES in HttpParseHeaders) or
// body-phase (exact remainder via WSK_FLAG_WAITALL, into
// Buffer or the caller's MDL). Checks remaining stack first and expands via
// callout if low -- see the stack-safety discussion above; Wait = FALSE is
// required at DISPATCH_LEVEL (Wait = TRUE there returns
// STATUS_INVALID_PARAMETER_4), and the callout is documented as not having
// been invoked when expansion fails, so Ctx is untouched in that case --
// but after a successful callout, the callout (and everything it did,
// possibly including completing this request) has already run and Ctx may
// already be freed. Two receive regimes are selected by whether headers
// have been parsed yet (BodyOffset != 0): header phase posts the
// remaining capacity and completes on whatever arrives (Flags = 0),
// growing a page at a time if the headers alone overflow it -- and only
// re-posting while HttpParseHeaders keeps answering STATUS_BUFFER_TOO_SMALL,
// which it stops doing past HTTP_MAX_HEADER_BYTES; body phase
// posts exactly the outstanding remainder with WSK_FLAG_WAITALL -- one
// completion for the whole body rather than one per arriving segment,
// into either the caller's locked MDL (TargetMdl set, body byte i at MDL
// offset i) or Buffer, pre-grown to BodyEndOffset by HttpReadResponse.
// The posted length must be exact: WAITALL only completes when the buffer
// is filled or the connection breaks, so over-posting parks the request
// until the receive watchdog kills it. A short or failed WAITALL
// completion (peer close, cancellation) flows through HttpOnReceive ->
// HttpReadResponse, which re-issues or fails via the same length checks.
// After issuing the receive, STATUS_PENDING means HttpOnReceive runs later
// on a fresh dispatch; any other status means HttpOnReceive already ran
// synchronously inside the call (IoSetCompletionRoutine with
// InvokeOnSuccess/InvokeOnError both TRUE), so Ctx may already be freed.
//
static VOID HttpIssueReceive(HTTP_CONTEXT* Ctx)
{
    SIZE_T remainingStack = IoGetRemainingStackSize();

    if (remainingStack < HTTP_STACK_SAFETY_MARGIN)
    {
        NTSTATUS expandResult = KeExpandKernelStackAndCalloutEx(
            HttpIssueReceiveExpandedCallout,
            Ctx,
            HTTP_STACK_EXPAND_SIZE,
            FALSE,
            NULL);

        if (!NT_SUCCESS(expandResult))
        {
            BLORGFS_PRINT(
                "HttpIssueReceive() - KeExpandKernelStackAndCalloutEx failed: 0x%X\n",
                expandResult);

            HttpComplete(Ctx, STATUS_INSUFFICIENT_RESOURCES);
            return;
        }

        return;
    }

    NTSTATUS result;

    if (0 == Ctx->BodyOffset)
    {
        NTSTATUS growResult = HttpGrowBufferIfNeeded(Ctx, C_CAST(SIZE_T, Ctx->Length) + PAGE_SIZE);

        if (!NT_SUCCESS(growResult))
        {
            HttpComplete(Ctx, growResult);
            return;
        }

        result = BlorgReceiveWskAsync(
            Ctx->Socket,
            Ctx->Buffer + Ctx->Length,
            Ctx->Capacity - Ctx->Length,
            0,
            HttpOnReceive,
            Ctx);
    }
    else if (Ctx->TargetMdl)
    {
        result = BlorgReceiveWskAsyncMdl(
            Ctx->Socket,
            Ctx->TargetMdl,
            Ctx->Length - C_CAST(ULONG, Ctx->BodyOffset),
            C_CAST(ULONG, Ctx->BodyEndOffset) - Ctx->Length,
            WSK_FLAG_WAITALL,
            HttpOnReceive,
            Ctx);
    }
    else
    {
        result = BlorgReceiveWskAsync(
            Ctx->Socket,
            Ctx->Buffer + Ctx->Length,
            C_CAST(ULONG, Ctx->BodyEndOffset) - Ctx->Length,
            WSK_FLAG_WAITALL,
            HttpOnReceive,
            Ctx);
    }

    if (STATUS_PENDING != result)
    {
        HttpComplete(Ctx, result);
    }
}

//
// Completion for a plaintext receive: retries/fails on error or an early
// 0-byte close, otherwise accumulates BytesTransferred and advances to
// response parsing. A failure on a reused pooled connection before any
// response byte arrives is the idle-close race: HttpFailOrRetryReusedConnection
// retries once on a fresh connection (HttpTryRetryReusedConnection only
// retries while ctx->Length == 0, so this never re-reads partial data), or
// fails the request if not retryable. A 0-byte transfer means the peer
// closed early: if a complete response hasn't been parsed yet (headers not
// parsed, or body not yet complete per the same length test as the success
// path), that is either the classic idle-close race on a reused connection
// with nothing received yet (retried the same way), or an outright error;
// otherwise it is equivalent to "no more data needed".
//
static VOID HttpOnReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext)
{
    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, CompletionContext);

    if (!NT_SUCCESS(Status))
    {
        HttpFailOrRetryReusedConnection(ctx, Status);
        return;
    }

    if (0 == BytesTransferred)
    {
        if (0 == ctx->BodyOffset || ctx->Length < ctx->BodyEndOffset)
        {
            HttpFailOrRetryReusedConnection(ctx, STATUS_CONNECTION_DISCONNECTED);
            return;
        }
    }

    ctx->Length += C_CAST(ULONG, BytesTransferred);

    ctx->Stage = HttpStageReadResponse;
    HttpKick(ctx);
}

//
// TLS record-layer receive path -- used instead of
// HttpIssueReceive/HttpOnReceive once Socket->Tls.State ==
// TlsHandshakeComplete. HttpReadResponse and everything downstream of it
// (header parsing, body completion, dispatch) stay unaware TLS is
// involved: this path's only job is to turn "some number of on-wire TLS
// records" into "Ctx->Length more decrypted plaintext bytes in
// Buffer/TargetMdl", ending with the same ctx->Length += ...; ctx->Stage
// = HttpStageReadResponse; HttpKick(ctx); tail HttpOnReceive uses.
//
// Framing: TLS 1.3 records don't align with HTTP body bytes -- each one
// adds a 5-byte header plus (after decrypt) a 16-byte tag and an inner
// content-type byte of overhead. Rather than receiving each record's
// pieces with exact-length WSK ops (a header/ciphertext/tag receive
// triplet -- three IRPs and three watchdog timers per 16 KB record),
// ciphertext is received in bulk: one receive posts all remaining
// TlsRecvBuffer space and completes on whatever arrives, and the drain
// loop below then consumes every complete record already buffered
// without touching the wire again. Decryption adds no copy of its own:
// AES-GCM reads the ciphertext from TlsRecvBuffer and writes the
// plaintext straight to its destination (target MDL or Buffer), so the
// data movement is fused into the decrypt pass the CPU had to make
// anyway -- the same zero-extra-copy property the old in-place MDL
// decrypt had, minus its two extra receives per record.
//
// KeExpandKernelStackAndCalloutEx callout target: re-enters HttpIssueTlsReceive
// with more stack available, same reasoning as HttpIssueReceiveExpandedCallout.
//
static VOID HttpIssueTlsReceiveExpandedCallout(PVOID Parameter)
{
    HttpIssueTlsReceive(C_CAST(HTTP_CONTEXT*, Parameter));
}

//
// Drains buffered TLS records, then issues the next bulk ciphertext
// receive once no complete record remains. Entry point for the TLS
// receive path described above; reached from HttpKick's HttpStageReceive
// case, HttpReadResponse's re-issue, and HttpOnTlsReceive. Same
// stack-safety concern as HttpIssueReceive (each delivered record
// re-enters here via HttpReadResponse -> HttpIssueReceiveDispatch on the
// same frame, once per buffered record) -- see HTTP_STACK_SAFETY_MARGIN/
// HTTP_STACK_EXPAND_SIZE above for the sizing rationale.
//
// During the header phase the loop returns to the stage machine after
// delivering each application-data record's content, so
// HttpReadResponse's phase logic (header parse, spill copy, body
// pre-grow) runs at the first opportunity. Once the body destination is
// known (BodyOffset != 0), buffered records are drained back-to-back --
// each already decrypts straight to its final destination -- and the
// stage machine only runs again when the body is complete (or overrun);
// records that deliver nothing are consumed entirely inside the loop
// in both phases. Per record:
//
//  - Outer type 0x14 (change_cipher_spec, an RFC 8446 Appendix D.4
//    middlebox-compat no-op) is skipped without decryption -- it isn't
//    AEAD-protected at all -- but must still be consumed from the byte
//    stream. 0x15 (alert) is a hard failure; 0x17 (application_data) is
//    the only other legal outer type post-handshake.
//
//  - A 0x17 record is AEAD-decrypted (tag verified) straight from its
//    wire position to its destination: the target MDL at the current
//    body position when the whole inner plaintext fits the MDL's
//    remaining room, Buffer (grown first) in buffer mode or during the
//    header phase (BodyOffset == 0 gates the MDL branch, not just
//    TargetMdl -- headers and any body bytes in the same record must
//    land in Buffer, HttpReadResponse's spill copy moves them), or
//    TlsPlaintextScratch for the one record whose inner plaintext may
//    overhang the MDL's end (normally the last -- earlier records'
//    content-type/padding overhang is overwritten by the next record's
//    plaintext at the same offset), whose real content is then copied.
//    BlorgTlsStripInnerPlaintext finds the inner content type and real length
//    by a backward scan, no data movement: Length advances by contentLen
//    only.
//
//  - Inner type 0x16 (post-handshake handshake message, e.g.
//    NewSessionTicket, or a KeyUpdate this driver does not implement) is
//    discarded without advancing Length: no session-resumption cache
//    exists, so NewSessionTicket is useless, and a KeyUpdate would leave
//    ReadKeyHandle stale, which fails closed (AEAD tag mismatch on the
//    next real record) rather than silently misreading data. 0x17 is the
//    only other legal inner type here.
//
// Framing/AEAD failures inside the loop route through
// HttpFailOrRetryReusedConnection rather than failing outright. The
// accumulator lives on the KSOCKET, so ciphertext a bulk receive pulls
// in past this response's boundary (e.g. a late NewSessionTicket) stays
// buffered on the connection and is drained by whichever request reuses
// it -- over-receiving no longer desyncs the read sequence the way the
// old per-request accumulator's discard-on-free did. The retry is kept
// for what remains: a pooled connection the peer corrupted or tore down
// mid-record still surfaces as a framing/decrypt failure before any
// plaintext has been delivered. On a fresh connection the same call
// degrades to a hard failure. The one exception is the tail-spill
// bound (real content overrunning the MDL), which is a provably
// misbehaving server, not connection state.
//
// When no complete record remains, the accumulator cursors are reset
// (fully drained) or the partial tail is compacted to the front of
// TlsRecvBuffer -- lazily, only once the free tail can no longer hold a
// max-size record -- and one bulk receive is posted for all remaining
// space with Flags = 0 (completes on arrival, unlike the plaintext body
// phase's exact-length WAITALL, since record boundaries can't be known
// before the headers they carry arrive) through the socket's prebuilt
// accumulator MDL. After issuing the receive, same STATUS_PENDING-vs-
// synchronous-completion reasoning as HttpIssueReceive applies.
//
static VOID HttpIssueTlsReceive(HTTP_CONTEXT* Ctx)
{
    SIZE_T remainingStack = IoGetRemainingStackSize();

    if (remainingStack < HTTP_STACK_SAFETY_MARGIN)
    {
        NTSTATUS expandResult = KeExpandKernelStackAndCalloutEx(
            HttpIssueTlsReceiveExpandedCallout,
            Ctx,
            HTTP_STACK_EXPAND_SIZE,
            FALSE,
            NULL);

        if (!NT_SUCCESS(expandResult))
        {
            HttpComplete(Ctx, STATUS_INSUFFICIENT_RESOURCES);
        }

        return;
    }

    PKSOCKET socket = Ctx->Socket;

    NTSTATUS accumulatorStatus = BlorgEnsureTlsRecvBuffer(socket);

    if (!NT_SUCCESS(accumulatorStatus))
    {
        HttpComplete(Ctx, accumulatorStatus);
        return;
    }

    for (;;)
    {
        PUCHAR record = socket->TlsRecvBuffer + socket->TlsRecvOffset;
        ULONG buffered = socket->TlsRecvLength - socket->TlsRecvOffset;

        if (buffered < 5)
        {
            break;
        }

        UCHAR recordType = record[0];
        ULONG declaredLen = (C_CAST(ULONG, record[3]) << 8) | C_CAST(ULONG, record[4]);

        if (0x15 == recordType)
        {
            HttpFail(Ctx, STATUS_CONNECTION_RESET);
            return;
        }

        if (declaredLen > TLS_RECORD_CIPHERTEXT_MAX ||
            (0x17 != recordType && 0x14 != recordType))
        {
            HttpFailOrRetryReusedConnection(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        if (buffered < 5 + declaredLen)
        {
            break;
        }

        if (0x14 == recordType)
        {
            socket->TlsRecvOffset += 5 + declaredLen;
            continue;
        }

        if (declaredLen < TLS_TAG_LEN + 1)
        {
            HttpFailOrRetryReusedConnection(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        ULONG innerLen = declaredLen - TLS_TAG_LEN;
        PUCHAR plaintext;
        PUCHAR spillDest = NULL;
        ULONG spillRoom = 0;

        if (Ctx->TargetMdl && Ctx->BodyOffset)
        {
            if (Ctx->Length > Ctx->BodyEndOffset)
            {
                HttpFail(Ctx, STATUS_INVALID_PARAMETER);
                return;
            }

            PVOID targetVa = MmGetSystemAddressForMdlSafe(Ctx->TargetMdl, NormalPagePriority | MdlMappingNoExecute);

            if (!targetVa)
            {
                HttpFail(Ctx, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }

            ULONG mdlRoom = C_CAST(ULONG, Ctx->BodyEndOffset) - Ctx->Length;
            PUCHAR mdlDest = C_CAST(PUCHAR, targetVa) + (Ctx->Length - C_CAST(ULONG, Ctx->BodyOffset));

            if (innerLen <= mdlRoom)
            {
                plaintext = mdlDest;
            }
            else
            {
                plaintext = socket->TlsPlaintextScratch;
                spillDest = mdlDest;
                spillRoom = mdlRoom;
            }
        }
        else
        {
            NTSTATUS growResult = HttpGrowBufferIfNeeded(Ctx, C_CAST(SIZE_T, Ctx->Length) + innerLen);

            if (!NT_SUCCESS(growResult))
            {
                HttpFail(Ctx, growResult);
                return;
            }

            plaintext = C_CAST(PUCHAR, Ctx->Buffer) + Ctx->Length;
        }

        NTSTATUS decStatus = BlorgTlsAeadDecryptKeyed(
            socket->Tls.ReadKeyHandle, socket->Tls.ReadIv, socket->Tls.ReadSeq,
            record, 5,
            record + 5, innerLen, record + 5 + innerLen,
            plaintext);

        if (!NT_SUCCESS(decStatus))
        {
            HttpFailOrRetryReusedConnection(Ctx, decStatus);
            return;
        }

        socket->Tls.ReadSeq++;
        socket->TlsRecvOffset += 5 + declaredLen;

        BLORGFS_STAT_INC(TlsRecordsDecrypted);
        BLORGFS_STAT_ADD(TlsBytesDecrypted, innerLen);

        UCHAR contentType;
        ULONG contentLen;

        if (!BlorgTlsStripInnerPlaintext(plaintext, innerLen, &contentType, &contentLen))
        {
            HttpFailOrRetryReusedConnection(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        if (0x16 == contentType)
        {
            continue;
        }

        if (0x17 != contentType)
        {
            HttpFailOrRetryReusedConnection(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        if (spillDest)
        {
            if (contentLen > spillRoom)
            {
                HttpFail(Ctx, STATUS_INVALID_PARAMETER);
                return;
            }

            RtlCopyMemory(spillDest, socket->TlsPlaintextScratch, contentLen);
        }

        Ctx->Length += contentLen;

        if (0 == Ctx->BodyOffset || Ctx->Length >= Ctx->BodyEndOffset)
        {
            Ctx->Stage = HttpStageReadResponse;
            HttpKick(Ctx);
            return;
        }
    }

    if (socket->TlsRecvOffset == socket->TlsRecvLength)
    {
        socket->TlsRecvLength = 0;
        socket->TlsRecvOffset = 0;
    }
    else if (socket->TlsRecvOffset &&
        (SocketTlsRecvCapacity - socket->TlsRecvLength) < (5 + TLS_RECORD_CIPHERTEXT_MAX))
    {
        RtlMoveMemory(
            socket->TlsRecvBuffer,
            socket->TlsRecvBuffer + socket->TlsRecvOffset,
            socket->TlsRecvLength - socket->TlsRecvOffset);

        socket->TlsRecvLength -= socket->TlsRecvOffset;
        socket->TlsRecvOffset = 0;
    }

    BLORGFS_STAT_INC(TlsBulkReceives);

    NTSTATUS result = BlorgReceiveWskAsyncMdl(
        socket,
        socket->TlsRecvMdl,
        socket->TlsRecvLength,
        SocketTlsRecvCapacity - socket->TlsRecvLength,
        0,
        HttpOnTlsReceive,
        Ctx);

    if (STATUS_PENDING != result)
    {
        HttpComplete(Ctx, result);
    }
}

//
// Completion for a bulk TLS ciphertext receive: retries/fails on error or
// an early close, otherwise accounts the newly arrived ciphertext and
// re-enters the drain loop. Failure and 0-byte handling mirror
// HttpOnReceive -- a failure or close on a reused pooled connection
// before any response byte was delivered is the idle-close race
// (HttpTryRetryReusedConnection also resets the ciphertext accumulator).
// Unlike HttpOnReceive there is no complete-response carve-out for the
// 0-byte case: a bulk receive is only ever posted while the drain loop
// still needs plaintext, so a close here is always premature.
//
static VOID HttpOnTlsReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID CompletionContext)
{
    HTTP_CONTEXT* ctx = C_CAST(HTTP_CONTEXT*, CompletionContext);

    if (!NT_SUCCESS(Status))
    {
        HttpFailOrRetryReusedConnection(ctx, Status);
        return;
    }

    if (0 == BytesTransferred)
    {
        HttpFailOrRetryReusedConnection(ctx, STATUS_CONNECTION_DISCONNECTED);
        return;
    }

    ctx->Socket->TlsRecvLength += C_CAST(ULONG, BytesTransferred);

    HttpIssueTlsReceive(ctx);
}

//
// Runs inline at <= DISPATCH_LEVEL on the WSK completion chain for every
// operation -- deliberately no PASSIVE bounce here. Everything this
// function touches directly is DISPATCH-safe by construction:
// phr_parse_response is pure C in the driver's non-paged text,
// HttpHeaderNameEquals exists precisely because RtlEqualString is not
// DISPATCH-safe, buffer growth is NonPagedPoolNx, and socket pool
// release is a spinlock. The work that does require PASSIVE_LEVEL --
// flatcc deserialization and the dir/file-info completion callbacks --
// sits entirely behind the HttpDispatch and HttpComplete gates (see
// HttpMustBounceToPassive), so those ops pay exactly one PASSIVE hop per
// request there, instead of one per receive completion here.
//
// A HttpOpFileRead 206 response whose Content-Length doesn't match
// ExpectedContentLength is checked independently of, and in addition to,
// HTTP_MAX_CONTENT_LENGTH: a value can be well under the general policy
// ceiling and still be wrong for this specific request, e.g. a
// misbehaving/malicious server ignoring the Range header and returning a
// larger body than asked for -- treated as a protocol violation rather
// than silently reading/allocating for whatever size it sent. In
// zero-copy mode (TargetMdl set), the body belongs in the caller's MDL,
// not Buffer: whatever slice of it arrived piggybacked on the header
// receive is the only body data that will ever be in Buffer, and is
// moved into the MDL now (usually zero, when the server sends headers in
// their own segment) -- every subsequent receive lands in the MDL
// directly, so Buffer never grows past the headers.
//
// How many bytes have arrived is not the same thing as how many the peer
// declared, and the gap between the two is the hazard this function is
// ordered around. The Length > BodyEndOffset test sits above everything
// that moves a byte, deliberately: every destination below is sized from
// the *declared* Content-Length -- the caller's MDL from the range it
// asked for, Buffer from BodyOffset + ContentLength -- while Length is
// whatever the peer chose to put on the wire, and the header-phase
// receive posts the whole remaining capacity, so one burst can leave
// Length sitting at Capacity with a Content-Length of four. Both
// destinations then copy Length - BodyOffset bytes: the MDL spill copy
// directly, and the flatcc alignment slide (below, metadata operations
// only) when the headers end off an 8-byte boundary and the body has to
// move down to the next one. Checking afterwards, as this used to, means
// the copy has already happened by the time the response is rejected.
// One test covers both destinations and both phases -- the same
// arithmetic decides "over-sent" during the header phase and after a body
// receive -- so it is not repeated per branch.
//
// Over-sending is a protocol violation that also desyncs a keep-alive
// stream, so it fails the request rather than truncating, matching how a
// short body is handled. In buffer mode, Buffer is then pre-grown to fit
// the full declared body now that ContentLength is known, so the
// remaining receive loop (if any) doesn't repeatedly realloc a page at a
// time for large files. Dispatch does not happen until the full declared
// Content-Length has arrived, looping HttpStageReceive as many times as
// the peer needs to deliver it.
//
static VOID HttpReadResponse(HTTP_CONTEXT* Ctx)
{
    BOOLEAN headersJustParsed = (0 == Ctx->BodyOffset);

    if (headersJustParsed)
    {
        NTSTATUS parseStatus = HttpParseHeaders(Ctx);

        if (STATUS_BUFFER_TOO_SMALL == parseStatus)
        {
            HttpIssueReceiveDispatch(Ctx);
            return;
        }

        if (!NT_SUCCESS(parseStatus))
        {
            HttpFail(Ctx, parseStatus);
            return;
        }

        if (Ctx->StatusCode != Ctx->ExpectedStatusCode)
        {
            HttpFail(Ctx, (404 == Ctx->StatusCode) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_INVALID_PARAMETER);
            return;
        }

        if (HttpOpFileRead == Ctx->Operation && Ctx->ContentLength != Ctx->ExpectedContentLength)
        {
            BLORGFS_PRINT(
                "HttpOnReceive() - Content-Length %Iu does not match requested range size %Iu, rejecting\n",
                Ctx->ContentLength,
                Ctx->ExpectedContentLength);

            HttpFail(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        Ctx->HeadersQpc = BlorgStatisticsNow();
    }

    if (Ctx->Length > Ctx->BodyEndOffset)
    {
        BLORGFS_PRINT(
            "HttpReadResponse() - peer sent %Iu bytes for a response ending at %Iu, rejecting\n",
            C_CAST(SIZE_T, Ctx->Length),
            Ctx->BodyEndOffset);

        HttpFail(Ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    if (headersJustParsed)
    {
        if (Ctx->TargetMdl)
        {
            SIZE_T spill = Ctx->Length - Ctx->BodyOffset;

            if (spill)
            {
                PVOID targetVa = MmGetSystemAddressForMdlSafe(
                    Ctx->TargetMdl,
                    NormalPagePriority | MdlMappingNoExecute);

                if (!targetVa)
                {
                    HttpFail(Ctx, STATUS_INSUFFICIENT_RESOURCES);
                    return;
                }

                RtlCopyMemory(targetVa, Ctx->Buffer + Ctx->BodyOffset, spill);
            }
        }
        else
        {
            if (HttpOpFileRead != Ctx->Operation)
            {
                SIZE_T alignedBodyOffset = (Ctx->BodyOffset + 7) & ~C_CAST(SIZE_T, 7);

                if (alignedBodyOffset != Ctx->BodyOffset)
                {
                    SIZE_T alignedBodyEnd;

                    if (!HttpCheckedAddSizeT(alignedBodyOffset, Ctx->ContentLength, &alignedBodyEnd))
                    {
                        HttpFail(Ctx, STATUS_INVALID_PARAMETER);
                        return;
                    }

                    NTSTATUS alignGrowResult = HttpGrowBufferIfNeeded(Ctx, alignedBodyEnd);

                    if (!NT_SUCCESS(alignGrowResult))
                    {
                        HttpFail(Ctx, alignGrowResult);
                        return;
                    }

                    RtlMoveMemory(
                        Ctx->Buffer + alignedBodyOffset,
                        Ctx->Buffer + Ctx->BodyOffset,
                        Ctx->Length - Ctx->BodyOffset);

                    Ctx->Length += C_CAST(ULONG, alignedBodyOffset - Ctx->BodyOffset);
                    Ctx->BodyOffset = alignedBodyOffset;
                    Ctx->BodyEndOffset = alignedBodyEnd;
                }
            }

            NTSTATUS growResult = HttpGrowBufferIfNeeded(Ctx, Ctx->BodyEndOffset);

            if (!NT_SUCCESS(growResult))
            {
                HttpFail(Ctx, growResult);
                return;
            }
        }
    }

    if (Ctx->Length < Ctx->BodyEndOffset)
    {
        HttpIssueReceiveDispatch(Ctx);
        return;
    }

    BlorgReleaseReusableWskSocket(Ctx->Socket);
    Ctx->Socket = NULL;

    Ctx->Stage = HttpStageDispatch;
    HttpKick(Ctx);
}

//
// Parses HTTP response headers out of Ctx->Buffer via picohttpparser,
// resolves Content-Length, and computes BodyOffset/BodyEndOffset. Returns
// STATUS_BUFFER_TOO_SMALL if more bytes are needed for the headers alone --
// unless HTTP_MAX_HEADER_BYTES has already accumulated, in which case the
// answer is STATUS_INVALID_NETWORK_RESPONSE and the request fails. This is
// the only place that distinction can be drawn: "incomplete" is the signal
// that makes the caller post another receive and grow the buffer again, so
// a peer that never terminates its headers is bounded here or nowhere.
// A Content-Length over HTTP_MAX_CONTENT_LENGTH is rejected outright
// rather than truncated or silently capped: truncating would let a
// malicious/buggy server cause this code to read or dispatch a body
// shorter than what was actually sent, its own class of correctness bug
// for a length-prefixed protocol, and a response this large is either a
// misbehaving peer or an attempt to exhaust kernel pool -- both correctly
// handled by failing the single request. The BodyOffset + ContentLength
// overflow check is a hard backstop that should be unreachable in
// practice given that ceiling (BodyOffset is bounded by the receive
// buffer size, itself bounded well below SIZE_MAX), but every later use
// of this sum (buffer sizing in HttpIssueReceive, the body completion
// test in HttpOnReceive) depends on it never having silently wrapped --
// defense in depth for a value that originates on the wire from a peer
// this driver does not fully trust.
//
static NTSTATUS HttpParseHeaders(HTTP_CONTEXT* Ctx)
{
    const char* msg;
    SIZE_T msgLen;
    SIZE_T headerCount = HTTP_MAX_HEADERS;

    int bytesProcessed = phr_parse_response(
        Ctx->Buffer,
        Ctx->Length,
        &Ctx->MinorVersion,
        &Ctx->StatusCode,
        &msg,
        &msgLen,
        Ctx->Headers,
        &headerCount,
        0);

    if (-2 == bytesProcessed)
    {
        return (Ctx->Length >= HTTP_MAX_HEADER_BYTES)
            ? STATUS_INVALID_NETWORK_RESPONSE
            : STATUS_BUFFER_TOO_SMALL;
    }

    if (bytesProcessed < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Ctx->HeaderCount = headerCount;
    Ctx->BodyOffset = C_CAST(SIZE_T, bytesProcessed);

    SIZE_T contentLength = 0;
    NTSTATUS status = GetContentLengthFromHeaders(Ctx->Headers, Ctx->HeaderCount, &contentLength);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (contentLength > HTTP_MAX_CONTENT_LENGTH)
    {
        BLORGFS_PRINT(
            "HttpParseHeaders() - Content-Length %Iu exceeds policy maximum %Iu, rejecting\n",
            contentLength,
            C_CAST(SIZE_T, HTTP_MAX_CONTENT_LENGTH));

        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T bodyEndOffset;

    if (!HttpCheckedAddSizeT(Ctx->BodyOffset, contentLength, &bodyEndOffset))
    {
        BLORGFS_PRINT("HttpParseHeaders() - BodyOffset + ContentLength overflow, rejecting\n");
        return STATUS_INVALID_PARAMETER;
    }

    Ctx->ContentLength = contentLength;
    Ctx->BodyEndOffset = bodyEndOffset;

    return STATUS_SUCCESS;
}

//
// Deserializes the response body per Ctx->Operation and fires the caller's
// completion callback on success, clearing Completion.*.Routine so
// HttpComplete does not invoke it a second time (dirInfo ownership
// transfers to the caller, who frees it with BlorgFreeHttpDirectoryInfo). Must
// run at PASSIVE (see HttpDispatch/HttpMustBounceToPassive) since flatcc
// and the callbacks require it. For HttpOpFileRead in zero-copy mode, the
// body is already in the caller's MDL, so there is nothing to hand over --
// BodyBuffer/BaseAddress are NULL (BlorgFreeHttpFile on a NULL BaseAddress is a
// no-op) and only the byte count is meaningful; in buffer mode, ownership
// of Ctx->Buffer transfers to the caller via BaseAddress (see
// HttpFreeContext).
//
static VOID HttpDispatchInline(HTTP_CONTEXT* Ctx)
{
    NTSTATUS result = STATUS_SUCCESS;

    switch (Ctx->Operation)
    {
    case HttpOpDirInfo:
    {
        PDIRECTORY_INFO dirInfo = NULL;
        result = HttpDeserializeDirectoryInfo(Ctx, &dirInfo);

        if (NT_SUCCESS(result))
        {
            Ctx->Completion.DirInfo.Routine(STATUS_SUCCESS, dirInfo, Ctx->CallerContext);
            Ctx->Completion.DirInfo.Routine = NULL;
        }

        break;
    }

    case HttpOpFileInfo:
    {
        DIRECTORY_ENTRY_METADATA dirEntInfo = { 0 };
        result = HttpDeserializeDirectoryEntryInfo(Ctx, &dirEntInfo);

        if (NT_SUCCESS(result))
        {
            Ctx->Completion.FileInfo.Routine(STATUS_SUCCESS, &dirEntInfo, Ctx->CallerContext);
            Ctx->Completion.FileInfo.Routine = NULL;
        }

        break;
    }

    case HttpOpFileRead:
    {
        FILE_BUFFER fileBuffer =
        {
            .BodyBuffer = Ctx->TargetMdl ? NULL : Ctx->Buffer + Ctx->BodyOffset,
            .BodyBufferSize = Ctx->ContentLength,
            .BaseAddress = Ctx->TargetMdl ? NULL : Ctx->Buffer
        };

        Ctx->Completion.FileRead.Routine(STATUS_SUCCESS, &fileBuffer, Ctx->CallerContext);
        Ctx->Completion.FileRead.Routine = NULL;

        break;
    }
    }

    if (!NT_SUCCESS(result))
    {
        HttpFail(Ctx, result);
        return;
    }

    Ctx->Stage = HttpStageComplete;
    HttpKick(Ctx);
}

//
// PASSIVE-level work-item target for HttpDispatch's bounce; just re-enters
// HttpDispatchInline, which drives the request to completion and frees the
// context (its work item included) -- nothing here may touch Context
// afterwards.
//
static VOID HttpDispatchWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    HttpDispatchInline(Context);
}

//
// In practice only failure paths reach here above PASSIVE for the
// deserializing ops (their success path already went through
// HttpReadResponse's bounce), but the gate is kept symmetric rather than
// reasoned away -- see HttpMustBounceToPassive.
//
static VOID HttpDispatch(HTTP_CONTEXT* Ctx)
{
    if (HttpMustBounceToPassive(Ctx))
    {
        IoQueueWorkItem(Ctx->WorkItem, HttpDispatchWorker, DelayedWorkQueue, Ctx);
        return;
    }

    HttpDispatchInline(Ctx);
}

//
// PASSIVE-level work-item target for HttpComplete's bounce; re-enters
// HttpComplete with the final status already recorded on Ctx. Re-entry at
// PASSIVE_LEVEL falls through the gate; HttpComplete frees the context
// (work item included) -- nothing after this call may touch Context.
//
static VOID HttpCompleteWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    HTTP_CONTEXT* ctx = Context;

    HttpComplete(ctx, ctx->FinalStatus);
}

//
// Terminal handler for both success and failure: bounces to PASSIVE if
// needed, fires the failure completion callback if Status is an error
// (success callbacks already fired in HttpDispatchInline), closes or frees
// the socket, and frees Ctx via HttpFreeContext. A failed dir/file-info
// request reaching here at DISPATCH_LEVEL (socket error, receive timeout,
// protocol violation -- any HttpFail fired straight from a WSK completion)
// must not invoke its completion callback yet: those callbacks take push
// locks inside KeEnterCriticalRegion (PathCache, DCB listing), which is
// APC_LEVEL-or-below work, so this hops to PASSIVE first as the
// failure-path counterpart of HttpReadResponse's bounce. On failure, the
// appropriate callback is fired here because HttpDispatch only clears
// Completion.*.Routine on the success path, so on any failure path
// (including ones that bypassed HttpDispatch entirely, e.g.
// socket/send/receive errors) the routine is still set and must be
// invoked exactly once. Any failure path that still holds a socket
// reference did not go through the request/response cycle cleanly, so the
// socket is closed rather than pooled.
//
// The per-phase spans are computed once here and used twice: for the
// latency sums, and for the outlier record. Whether that record is kept at
// all is decided inside BlorgStatisticsRecordSlowFetch by a raw-tick
// threshold compare, so a fetch that is not an outlier pays nothing beyond
// the call.
//
static VOID HttpComplete(HTTP_CONTEXT* Ctx, NTSTATUS Status)
{
    Ctx->FinalStatus = Status;

    if (HttpMustBounceToPassive(Ctx))
    {
        IoQueueWorkItem(Ctx->WorkItem, HttpCompleteWorker, DelayedWorkQueue, Ctx);
        return;
    }

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (statsBlock && HttpOpFileRead == Ctx->Operation && 0 != Ctx->HeadersQpc)
    {
        if (0 != Ctx->SendQpc)
        {
            BlorgStatisticsRecordLatency(
                &statsBlock->FetchPreSendSumUs,
                &statsBlock->FetchPreSendMaxUs,
                NULL,
                Ctx->SendQpc - Ctx->IssueQpc);
        }

        if (0 != Ctx->SocketQpc)
        {
            BlorgStatisticsRecordLatency(
                &statsBlock->FetchAcquireSumUs,
                &statsBlock->FetchAcquireMaxUs,
                NULL,
                Ctx->SocketQpc - Ctx->IssueQpc);

            if (HttpConnectionFresh == Ctx->ConnectionSource)
            {
                statsBlock->FetchFreshConnects++;

                BlorgStatisticsRecordLatency(
                    &statsBlock->FetchFreshAcquireSumUs,
                    &statsBlock->FetchFreshAcquireMaxUs,
                    NULL,
                    Ctx->SocketQpc - Ctx->IssueQpc);
            }
        }

        if (0 != Ctx->SendDoneQpc && 0 != Ctx->SendQpc)
        {
            BlorgStatisticsRecordLatency(
                &statsBlock->FetchSendSumUs,
                &statsBlock->FetchSendMaxUs,
                NULL,
                Ctx->SendDoneQpc - Ctx->SendQpc);

            BlorgStatisticsRecordLatency(
                &statsBlock->FetchWaitSumUs,
                &statsBlock->FetchWaitMaxUs,
                NULL,
                Ctx->HeadersQpc - Ctx->SendDoneQpc);
        }

        if (0 != Ctx->SendIssuedQpc && 0 != Ctx->SendQpc)
        {
            BlorgStatisticsRecordLatency(
                &statsBlock->FetchSendSubmitSumUs,
                &statsBlock->FetchSendSubmitMaxUs,
                NULL,
                Ctx->SendIssuedQpc - Ctx->SendQpc);
        }

        if (0 != Ctx->SendDoneQpc && 0 != Ctx->SendIssuedQpc)
        {
            BlorgStatisticsRecordLatency(
                &statsBlock->FetchSendSettleSumUs,
                &statsBlock->FetchSendSettleMaxUs,
                NULL,
                Ctx->SendDoneQpc - Ctx->SendIssuedQpc);
        }

        BlorgStatisticsRecordLatency(
            &statsBlock->FetchTtfbSumUs,
            &statsBlock->FetchTtfbMaxUs,
            NULL,
            Ctx->HeadersQpc - Ctx->IssueQpc);

        const LONG64 completedQpc = BlorgStatisticsNow();

        BlorgStatisticsRecordLatency(
            &statsBlock->FetchBodySumUs,
            &statsBlock->FetchBodyMaxUs,
            NULL,
            completedQpc - Ctx->HeadersQpc);

        statsBlock->FetchSplitSamples++;

        BlorgStatisticsRecordSlowFetch(
            completedQpc - Ctx->IssueQpc,
            (0 != Ctx->SocketQpc)
                ? Ctx->SocketQpc - Ctx->IssueQpc : 0,
            (0 != Ctx->SendDoneQpc && 0 != Ctx->SendQpc)
                ? Ctx->SendDoneQpc - Ctx->SendQpc : 0,
            (0 != Ctx->SendDoneQpc)
                ? Ctx->HeadersQpc - Ctx->SendDoneQpc : 0,
            Ctx->HeadersQpc - Ctx->IssueQpc,
            completedQpc - Ctx->HeadersQpc,
            Ctx->ExpectedContentLength,
            C_CAST(BOOLEAN, HttpConnectionFresh != Ctx->ConnectionSource));
    }

    if (statsBlock && HttpOpFileRead != Ctx->Operation)
    {
        ULONG64* latencySum = (HttpOpDirInfo == Ctx->Operation)
            ? &statsBlock->DirInfoLatencySumUs
            : &statsBlock->FileInfoLatencySumUs;

        ULONG64 discardedMax = 0;

        BlorgStatisticsRecordLatency(
            latencySum,
            &discardedMax,
            NULL,
            BlorgStatisticsNow() - Ctx->IssueQpc);

        if (NT_SUCCESS(Status))
        {
            statsBlock->MetaDataReadBytes += Ctx->ContentLength;
        }
        else if (HttpOpDirInfo == Ctx->Operation)
        {
            statsBlock->DirInfoFailures++;
        }
        else
        {
            statsBlock->FileInfoFailures++;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        switch (Ctx->Operation)
        {
            case HttpOpDirInfo:
            {
                if (Ctx->Completion.DirInfo.Routine)
                {
                    Ctx->Completion.DirInfo.Routine(Status, NULL, Ctx->CallerContext);
                }

                break;
            }
            case HttpOpFileInfo:
            {
                if (Ctx->Completion.FileInfo.Routine)
                {
                    Ctx->Completion.FileInfo.Routine(Status, NULL, Ctx->CallerContext);
                }

                break;
            }
            case HttpOpFileRead:
            {
                if (Ctx->Completion.FileRead.Routine)
                {
                    Ctx->Completion.FileRead.Routine(Status, NULL, Ctx->CallerContext);
                }

                break;
            }
        }

        if (Ctx->Socket)
        {
            BlorgCloseWskSocketAsync(Ctx->Socket);
            Ctx->Socket = NULL;
        }
    }

    HttpFreeContext(Ctx);
}

///////////////////////////////////////////////////////////////////////////
// Public entry points
///////////////////////////////////////////////////////////////////////////

//
// Formats a request line + headers into Ctx->RequestBuffer from a caller
// -supplied format string.
//
// PASSIVE_LEVEL only, and still so after the path stopped being formatted
// as %wZ: UrlEncodePathToAnsi's RtlUnicodeStringToUTF8String is itself
// paged-code, so no request may ever be issued above PASSIVE. Today every
// issue path (create/dir-control/read FSP workers)
// already is, and the driver's issuance rule names this conversion as the
// reason. What changed is only that the format string no longer adds a
// second reason of its own.
//
// The buffer-size budget counts the format string (whose specifier
// characters cover their own replacement overhead), the URL-encoded path's
// length -- now its exact byte count rather than twice it, since the
// encoder emits the ANSI the formatter consumes -- the caller's digit
// budget, and global.RemoteHostAnsi (bounded by
// BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES, Driver.h).
//

//
// Bounds RtlStringCbLengthA's scan of a caller-supplied FormatString
// literal in HttpBuildRequest below; every request format in this file is
// well under 256 bytes.
//
#define HTTP_BUILD_REQUEST_FORMAT_STRING_MAX_BYTES 256

static NTSTATUS HttpBuildRequest(
    const UNICODE_STRING* Path,
    const char* FormatString,
    SIZE_T ExtraDigitsBudget,
    SIZE_T StartOffset,
    SIZE_T EndOffsetInclusive,
    BOOLEAN IsRangedRequest,
    HTTP_CONTEXT* Ctx
)
{
    NTSTATUS result = UrlEncodePathToAnsi(Path, &Ctx->EncodedPathBuffer);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    size_t remoteHostLength;
    result = RtlStringCbLengthA(global.RemoteHostAnsi, BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES, &remoteHostLength);

    if (!NT_SUCCESS(result))
    {
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
        RtlZeroMemory(&Ctx->EncodedPathBuffer, sizeof(ANSI_STRING));
        return result;
    }

    size_t formatStringLength;
    result = RtlStringCbLengthA(FormatString, HTTP_BUILD_REQUEST_FORMAT_STRING_MAX_BYTES, &formatStringLength);

    if (!NT_SUCCESS(result))
    {
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
        RtlZeroMemory(&Ctx->EncodedPathBuffer, sizeof(ANSI_STRING));
        return result;
    }

    ULONG sendBufferSize = C_CAST(ULONG, formatStringLength) + 1 + Ctx->EncodedPathBuffer.Length + C_CAST(ULONG, ExtraDigitsBudget) + C_CAST(ULONG, remoteHostLength);

    Ctx->RequestBuffer = ExAllocatePoolZero(NonPagedPoolNx, sendBufferSize, 'BOOB');

    if (!Ctx->RequestBuffer)
    {
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
        RtlZeroMemory(&Ctx->EncodedPathBuffer, sizeof(ANSI_STRING));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (IsRangedRequest)
    {
        result = RtlStringCbPrintfA(Ctx->RequestBuffer, sendBufferSize, FormatString, Ctx->EncodedPathBuffer.Buffer, global.RemoteHostAnsi, StartOffset, EndOffsetInclusive);
    }
    else
    {
        result = RtlStringCbPrintfA(Ctx->RequestBuffer, sendBufferSize, FormatString, Ctx->EncodedPathBuffer.Buffer, global.RemoteHostAnsi);
    }

    if (!NT_SUCCESS(result))
    {
        ExFreePool(Ctx->RequestBuffer);
        Ctx->RequestBuffer = NULL;
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
        RtlZeroMemory(&Ctx->EncodedPathBuffer, sizeof(ANSI_STRING));
        return result;
    }

    size_t requestLength;
    result = RtlStringCbLengthA(Ctx->RequestBuffer, sendBufferSize, &requestLength);

    if (!NT_SUCCESS(result))
    {
        ExFreePool(Ctx->RequestBuffer);
        Ctx->RequestBuffer = NULL;
        ExFreePool(Ctx->EncodedPathBuffer.Buffer);
        RtlZeroMemory(&Ctx->EncodedPathBuffer, sizeof(ANSI_STRING));
        return result;
    }

    Ctx->RequestLength = C_CAST(ULONG, requestLength);

    return STATUS_SUCCESS;
}

//
// Allocates and initializes an HTTP_CONTEXT: the context struct, its
// initial receive buffer, and a preallocated work item (so later PASSIVE
// bounces can never fail for lack of one -- see the WorkItem field
// comment).
//
static HTTP_CONTEXT* HttpAllocateContext(HTTP_OPERATION Operation, int ExpectedStatusCode, SIZE_T InitialCapacity)
{
    if (!HttpAcquireActive())
    {
        return NULL;
    }

    HTTP_CONTEXT* ctx = ExAllocatePoolZero(NonPagedPoolNx, sizeof(HTTP_CONTEXT), HTTP_TAG);

    if (!ctx)
    {
        HttpReleaseActive();
        return NULL;
    }

    ctx->Buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, InitialCapacity, HTTP_TAG);

    if (!ctx->Buffer)
    {
        ExFreePool(ctx);
        HttpReleaseActive();
        return NULL;
    }

    if (HttpNeedsWorkItem(Operation))
    {
        ctx->WorkItem = IoAllocateWorkItem(global.FileSystemDeviceObject);

        if (!ctx->WorkItem)
        {
            ExFreePool(ctx->Buffer);
            ExFreePool(ctx);
            HttpReleaseActive();
            return NULL;
        }
    }

    ctx->Capacity = C_CAST(ULONG, InitialCapacity);
    ctx->IssueQpc = BlorgStatisticsNow();
    ctx->Operation = Operation;
    ctx->ExpectedStatusCode = ExpectedStatusCode;
    ctx->Stage = HttpStageAcquireSocket;

    RtlCopyMemory(&ctx->RemoteAddress, global.RemoteAddressInfo->ai_addr, global.RemoteAddressInfo->ai_addrlen);

    return ctx;
}

//
// Issues an async GET for a directory listing; CompletionRoutine is invoked
// exactly once with the deserialized result. On a HttpBuildRequest failure,
// cleanup always goes through HttpFreeContext rather than manual frees
// since the context also owns a preallocated work item; FinalStatus is set
// to the failure first because the zeroed default is a success status,
// which for a file read would make HttpFreeContext treat Buffer as
// caller-owned and leak it.
//
NTSTATUS BlorgHttpGetDirectoryInfo(
    const UNICODE_STRING* Path,
    PBLORG_DIRINFO_COMPLETION CompletionRoutine,
    PVOID CallerContext
)
{
    if (!Path || 0 == Path->Length || !Path->Buffer || !CompletionRoutine)
    {
        return STATUS_INVALID_PARAMETER;
    }

    HTTP_CONTEXT* ctx = HttpAllocateContext(HttpOpDirInfo, 200, HTTP_INITIAL_RECV_CAPACITY);

    if (!ctx)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->Completion.DirInfo.Routine = CompletionRoutine;
    ctx->CallerContext = CallerContext;

    BLORGFS_STAT_INC(DirInfoRequests);
    BLORGFS_STAT_INC(MetaDataDiskReads);

    static const char requestFormat[] =
        "GET /get_dir_info?path=%hs HTTP/1.1\r\n"
        "Host: %hs\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    NTSTATUS result = HttpBuildRequest(Path, requestFormat, 0, 0, 0, FALSE, ctx);

    if (!NT_SUCCESS(result))
    {
        ctx->FinalStatus = result;
        HttpFreeContext(ctx);
        return result;
    }

    HttpKick(ctx);
    return STATUS_PENDING;
}

//
// Issues an async GET for a single file/dir's stat metadata;
// CompletionRoutine is invoked exactly once with the deserialized result.
// See BlorgHttpGetDirectoryInfo for the HttpBuildRequest failure cleanup
// rationale.
//
NTSTATUS BlorgHttpGetFileInformation(
    const UNICODE_STRING* Path,
    PBLORG_FILEINFO_COMPLETION CompletionRoutine,
    PVOID CallerContext
)
{
    if (!Path || 0 == Path->Length || !Path->Buffer || !CompletionRoutine)
    {
        return STATUS_INVALID_PARAMETER;
    }

    HTTP_CONTEXT* ctx = HttpAllocateContext(HttpOpFileInfo, 200, PAGE_SIZE);

    if (!ctx)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->Completion.FileInfo.Routine = CompletionRoutine;
    ctx->CallerContext = CallerContext;

    BLORGFS_STAT_INC(FileInfoRequests);
    BLORGFS_STAT_INC(MetaDataDiskReads);

    static const char requestFormat[] =
        "GET /get_dir_entry_info?path=%hs HTTP/1.1\r\n"
        "Host: %hs\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    NTSTATUS result = HttpBuildRequest(Path, requestFormat, 0, 0, 0, FALSE, ctx);

    if (!NT_SUCCESS(result))
    {
        ctx->FinalStatus = result;
        HttpFreeContext(ctx);
        return result;
    }

    HttpKick(ctx);
    return STATUS_PENDING;
}

//
// Shared implementation behind BlorgHttpGetFile/BlorgHttpGetFileMdl: issues
// an async ranged GET for file content, into Buffer (TargetMdl NULL) or
// directly into the caller's locked MDL (zero-copy). endOffsetExclusive is
// StartOffset + Length; the Range header wants this minus 1. The check
// below rejects either an overflow in that sum, or the case where
// (StartOffset + Length) - 1 would itself underflow -- only possible if
// the checked add produced exactly 0, i.e. StartOffset == Length == 0,
// already excluded by the 0 == Length check above, but kept explicit
// rather than relying on that exclusion alone. Zero-copy requests never
// put body bytes in Buffer, so headers-only sizing suffices; buffer mode
// sizes for a full read-ahead chunk so the body fits without a regrow. On
// a HttpBuildRequest failure, see BlorgHttpGetDirectoryInfo for the
// HttpFreeContext/FinalStatus cleanup rationale.
//
static NTSTATUS HttpGetFileCommon(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PMDL TargetMdl,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext
)
{
    if (!Path || 0 == Path->Length || !Path->Buffer || !CompletionRoutine || 0 == Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T endOffsetExclusive;

    if (!HttpCheckedAddSizeT(StartOffset, Length, &endOffsetExclusive) || 0 == endOffsetExclusive)
    {
        return STATUS_INVALID_PARAMETER;
    }

    HTTP_CONTEXT* ctx = HttpAllocateContext(
        HttpOpFileRead,
        206,
        TargetMdl ? HTTP_MDL_INITIAL_RECV_CAPACITY : HTTP_FILE_INITIAL_RECV_CAPACITY);

    if (!ctx)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->TargetMdl = TargetMdl;
    ctx->Completion.FileRead.Routine = CompletionRoutine;
    ctx->CallerContext = CallerContext;
    ctx->ExpectedContentLength = Length;

    static const char requestFormat[] =
        "GET /get_file?path=%hs HTTP/1.1\r\n"
        "Host: %hs\r\n"
        "Connection: keep-alive\r\n"
        "Range: bytes=%zu-%zu\r\n"
        "\r\n";

    const SIZE_T ULLONG_MAX_DIGITS = 20;

    NTSTATUS result = HttpBuildRequest(
        Path,
        requestFormat,
        ULLONG_MAX_DIGITS * 2,
        StartOffset,
        endOffsetExclusive - 1,
        TRUE,
        ctx);

    if (!NT_SUCCESS(result))
    {
        ctx->FinalStatus = result;
        HttpFreeContext(ctx);
        return result;
    }

    HttpKick(ctx);
    return STATUS_PENDING;
}

NTSTATUS BlorgHttpGetFile(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext
)
{
    return HttpGetFileCommon(Path, StartOffset, Length, NULL, CompletionRoutine, CallerContext);
}

NTSTATUS BlorgHttpGetFileMdl(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PMDL TargetMdl,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext
)
{
    if (!TargetMdl)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return HttpGetFileCommon(Path, StartOffset, Length, TargetMdl, CompletionRoutine, CallerContext);
}

//
// Frees a PDIRECTORY_INFO returned via BlorgHttpGetDirectoryInfo's
// completion callback.
//
VOID BlorgFreeHttpDirectoryInfo(PDIRECTORY_INFO DirInfo)
{
    if (DirInfo)
    {
        ExFreePool(DirInfo);
    }
}

//
// Frees a FILE_BUFFER's BaseAddress from a buffer-mode BlorgHttpGetFile
// completion. No-op for zero-copy (MDL) reads, where BaseAddress is NULL.
//
VOID BlorgFreeHttpFile(PFILE_BUFFER FileBuffer)
{
    if (FileBuffer && FileBuffer->BaseAddress)
    {
        ExFreePool(FileBuffer->BaseAddress);
    }
}

//
// Thin wrapper over BlorgGetWskAddrInfo; exposes DNS resolution to callers
// outside Socket.c under the Client-facing naming.
//
NTSTATUS BlorgGetHttpAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo)
{
    return BlorgGetWskAddrInfo(NodeName, ServiceName, Hints, RemoteAddrInfo);
}

// Thin wrapper over BlorgFreeWskAddrInfo; frees results from BlorgGetHttpAddrInfo.
VOID BlorgFreeHttpAddrInfo(PADDRINFOEXW AddrInfo)
{
    BlorgFreeWskAddrInfo(AddrInfo);
}

//
// Thin wrapper over BlorgInitialiseWskClient; driver-load-time setup of the WSK
// transport this client runs on.
//
NTSTATUS BlorgInitialiseHttpClient(VOID)
{
    KeInitializeEvent(&HttpDrainEvent, NotificationEvent, FALSE);

    return BlorgInitialiseWskClient();
}

//
// Releases the standing reference and waits for every in-flight request
// to finish. From here on HttpAllocateContext refuses new requests, so
// the count is monotonically decreasing and the wait terminates as soon
// as the last outstanding completion runs -- bounded by the per-operation
// socket watchdogs (SOCKET_RECEIVE_TIMEOUT_MS), so a dead peer cannot
// hang unload indefinitely.
//
// Must run before the device objects are torn down: an outstanding
// request may still queue an IO work item against
// global.FileSystemDeviceObject.
//
VOID BlorgDrainHttpClient(VOID)
{
    HttpReleaseActive();

    KeWaitForSingleObject(&HttpDrainEvent, Executive, KernelMode, FALSE, NULL);
}

//
// Thin wrapper over BlorgCleanupWskClient; driver-unload-time teardown of the
// WSK transport.
//
VOID BlorgCleanupHttpClient(VOID)
{
    BlorgCleanupWskClient();
}
