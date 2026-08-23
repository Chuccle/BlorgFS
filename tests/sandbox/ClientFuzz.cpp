//
// Fuzz driver for the HTTP client's response-parsing surface.
//
// The client reads bytes it does not control: a status line, headers, a
// Content-Length it does arithmetic on, and a body it sizes buffers
// from. Every one of those is reachable by a peer, so the interesting
// question is not "does it work against our server" but "what does it do
// against any byte string at all".
//
// The input is used two ways, because the shape of the delivery matters
// as much as the bytes. The first byte selects a chunking pattern -- one
// blob, byte at a time, or pseudo-random splits -- and the rest is the
// response. Splitting the same bytes differently drives completely
// different paths through the header reassembly and the body loop, and a
// single-blob-only fuzzer would never reach them.
//
// Builds either as a libFuzzer target (define BLORGFS_LIBFUZZER and use
// a clang-cl toolchain) or, by default, as a standalone runner that
// replays a corpus directory and does bounded random mutation. The
// standalone mode exists so the sandbox is usable with the MSVC toolchain
// the rest of this repo builds under, rather than being gated on a
// second compiler.
//
// A finding here is anything that trips the sandbox's own guards: a pool
// overrun or double free (SandboxDriver.c fences every allocation), a
// leaked allocation, a completion callback that fires twice or never, or
// an outright crash. The client is allowed to reject any input it likes
// -- rejection is correct behaviour, not a finding.
//

//
// C linkage, matching ClientTest.cpp: this header now pulls in the
// driver's own Driver.h/Socket.h rather than declaring its own copies, and
// those declare C functions.
//
extern "C" {
#include "SandboxSocket.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace
{
    int CompletionCalls = 0;
    NTSTATUS CompletionStatus = STATUS_SUCCESS;

    void OnFileRead(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
    {
        (void)FileBuffer;
        (void)CallerContext;

        CompletionCalls++;
        CompletionStatus = Status;
    }

    //
    // At most this many chunks per input. A cap keeps one pathological
    // input from spending the whole fuzzing budget building a script.
    //
    const size_t kMaxSteps = 64;

    SANDBOX_STEP Steps[kMaxSteps];
}

//
// One fuzz iteration: build a delivery script from Data, run a ranged
// read against it, and assert only the contract -- exactly one outcome,
// and nothing leaked. Everything else is the client's business.
//
extern "C" int BlorgFuzzOnce(const unsigned char* Data, size_t Size)
{
    if (Size < 2)
    {
        return 0;
    }

    const unsigned char chunkMode = Data[0];
    const unsigned char* body = Data + 1;
    const size_t bodySize = Size - 1;

    SandboxInitialize();

    size_t stepCount = 0;
    size_t offset = 0;

    while (offset < bodySize && stepCount < kMaxSteps)
    {
        size_t take;

        switch (chunkMode & 0x03)
        {
            case 0:
            {
                // One blob: the common case, headers and body together.
                take = bodySize - offset;
                break;
            }
            case 1:
            {
                // A byte at a time: maximum reassembly pressure.
                take = 1;
                break;
            }
            case 2:
            {
                // Fixed small chunks straddling the header/body boundary.
                take = 7;
                break;
            }
            default:
            {
                //
                // Pseudo-random splits derived from the data itself, so
                // the split pattern is part of the reproducible input
                // rather than of the runner's RNG state.
                //
                take = 1 + (body[offset] % 29);
                break;
            }
        }

        if (take > bodySize - offset)
        {
            take = bodySize - offset;
        }

        Steps[stepCount].Kind = SandboxStepDeliver;
        Steps[stepCount].Data = body + offset;
        Steps[stepCount].Length = take;
        Steps[stepCount].Status = STATUS_SUCCESS;

        //
        // The high bits of the mode byte decide whether completions run
        // inline or deferred, which is what varies synchronous-chain
        // depth -- and with it, whether the stack-expansion path is
        // reached.
        //
        Steps[stepCount].Inline = ((chunkMode & 0x80) == 0) ? TRUE : FALSE;

        offset += take;
        stepCount++;
    }

    if (0 == stepCount)
    {
        SandboxCleanup();
        return 0;
    }

    SandboxSetPeerScript(Steps, stepCount);

    //
    // A squeezed stack budget so the expansion path is on the table for
    // every input rather than only the longest ones.
    //
    ShimSetRemainingStack(4096);

    unsigned char target[512];
    memset(target, 0, sizeof(target));

    PMDL mdl = ShimCreateMdl(target, sizeof(target));

    CompletionCalls = 0;

    wchar_t path[] = L"/fuzz/target.bin";
    UNICODE_STRING pathString;
    pathString.Buffer = path;
    pathString.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
    pathString.MaximumLength = pathString.Length;

    NTSTATUS status = BlorgHttpGetFileMdl(&pathString, 0, sizeof(target), mdl, OnFileRead, nullptr);

    SandboxDrainCompletions();
    ShimDrainWorkItems();

    int expected = (STATUS_PENDING == status) ? 1 : 0;

    if (CompletionCalls != expected)
    {
        fprintf(stderr, "[fuzz] completion contract violated: issue=0x%08lX callbacks=%d expected=%d\n",
            status, CompletionCalls, expected);
        abort();
    }

    ShimFreeMdl(mdl);
    BlorgCleanupWskClient();
    SandboxCleanup();

    if (ShimPoolOutstanding() != 0)
    {
        fprintf(stderr, "[fuzz] %zu pool allocation(s) leaked\n", ShimPoolOutstanding());
        abort();
    }

    return 0;
}

#ifdef BLORGFS_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* Data, size_t Size)
{
    return BlorgFuzzOnce(Data, Size);
}

#else

namespace
{
    //
    // Seeds chosen to put the fuzzer near the interesting boundaries
    // rather than making it discover HTTP from scratch: a well-formed
    // response, ones with the length fields at their limits, and the
    // shapes the scenario suite showed matter.
    //
    const char* const kSeeds[] =
    {
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n\r\nABCDEFGH",
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 512\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 99999999999999999999\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Length:\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\nContent-Length: 9\r\n\r\nABCDEFGH",
        "HTTP/1.1 200 OK\r\nContent-Length: 512\r\n\r\n",
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 999 Nonsense\r\nContent-Length: 1\r\n\r\nX",
        "HTTP/1.1 206\r\n\r\n",
        "\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\n\r\n",
        "NOT HTTP AT ALL",
    };

    unsigned int RandomState = 0x1234567u;

    unsigned int NextRandom()
    {
        RandomState = RandomState * 1664525u + 1013904223u;
        return RandomState >> 8;
    }

    //
    // Byte-level mutation only. A structure-aware mutator would produce
    // better-formed inputs, but the point of this pass is the malformed
    // ones -- and picohttpparser plus the length arithmetic is exactly
    // where a stray byte matters.
    //
    void Mutate(unsigned char* Buffer, size_t* Size, size_t Capacity)
    {
        const unsigned int operations = 1 + (NextRandom() % 4);

        for (unsigned int i = 0; i < operations; ++i)
        {
            if (*Size == 0)
            {
                return;
            }

            switch (NextRandom() % 4)
            {
                case 0:
                {
                    Buffer[NextRandom() % *Size] = (unsigned char)(NextRandom() & 0xFF);
                    break;
                }
                case 1:
                {
                    if (*Size + 1 < Capacity)
                    {
                        size_t at = NextRandom() % *Size;
                        memmove(Buffer + at + 1, Buffer + at, *Size - at);
                        Buffer[at] = (unsigned char)(NextRandom() & 0xFF);
                        (*Size)++;
                    }
                    break;
                }
                case 2:
                {
                    size_t at = NextRandom() % *Size;
                    memmove(Buffer + at, Buffer + at + 1, *Size - at - 1);
                    (*Size)--;
                    break;
                }
                default:
                {
                    size_t at = NextRandom() % *Size;
                    Buffer[at] = (unsigned char)('0' + (NextRandom() % 10));
                    break;
                }
            }
        }
    }
}

int main(int argc, char** argv)
{
    unsigned long iterations = 200000;

    if (argc > 1)
    {
        iterations = strtoul(argv[1], nullptr, 10);
    }

    printf("=== BlorgFS HTTP client fuzz ===\n");
    printf("  %lu iterations over %zu seeds\n\n", iterations, sizeof(kSeeds) / sizeof(kSeeds[0]));

    const size_t capacity = 2048;
    unsigned char* buffer = (unsigned char*)malloc(capacity);

    for (unsigned long i = 0; i < iterations; ++i)
    {
        const char* seed = kSeeds[NextRandom() % (sizeof(kSeeds) / sizeof(kSeeds[0]))];

        size_t size = strlen(seed);

        if (size + 1 > capacity)
        {
            size = capacity - 1;
        }

        //
        // Byte 0 is the delivery mode, so the fuzzer explores chunking
        // patterns alongside content.
        //
        buffer[0] = (unsigned char)(NextRandom() & 0xFF);
        memcpy(buffer + 1, seed, size);

        size_t total = size + 1;

        Mutate(buffer, &total, capacity);

        BlorgFuzzOnce(buffer, total);

        if (0 == (i % 20000) && i > 0)
        {
            printf("  %lu iterations, no findings\n", i);
        }
    }

    free(buffer);

    printf("\n=== %lu iterations, no crash, no leak, no contract violation ===\n", iterations);

    return 0;
}

#endif
