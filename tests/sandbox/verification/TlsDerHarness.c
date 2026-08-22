//
// CBMC proof harness for the DER reader in the real Tls.c.
//
// This includes the driver's own translation unit rather than a copy, so
// what CBMC proves is a property of the shipping code. The subject is the
// certificate parser's innermost primitive: it advances a cursor through
// attacker-supplied DER, in kernel mode, on bytes a compromised or hostile
// server chose. A read past End here is a kernel out-of-bounds read.
//
// CBMC explores every byte value and every length up to DER_HARNESS_BYTES
// exhaustively -- not a sample of them -- and its own --bounds-check and
// --pointer-check prove the absence of out-of-bounds access over that whole
// space. The explicit assertions below add the two contract properties the
// callers rely on and which no memory-safety check would catch on its own.
//

#include "..\..\Tls.c"

#define DER_HARNESS_BYTES 8

void TlsDerReadTlvHarness(void)
{
    UCHAR buffer[DER_HARNESS_BYTES];

    ULONG length;
    __CPROVER_assume(length <= DER_HARNESS_BYTES);

    UCHAR expectedTag;

    const UCHAR* cursor = buffer;
    const UCHAR* end = buffer + length;

    ULONG valueLength = 0;

    BOOLEAN parsed = TlsDerReadTlv(&cursor, end, expectedTag, &valueLength);

    //
    // The cursor must remain a valid position inside the buffer whatever
    // the input said, because every caller keeps parsing from it.
    //
    __CPROVER_assert(cursor >= buffer, "cursor never moves before the buffer");
    __CPROVER_assert(cursor <= end, "cursor never moves past the end");

    //
    // On success the reported value length must fit in what is left, or
    // the caller's subsequent read of the value walks off the end.
    //
    if (parsed)
    {
        __CPROVER_assert(valueLength <= (ULONG)(end - cursor),
            "reported value length fits within the remaining buffer");
    }

    //
    // Vacuity guard. Invoke-BlorgProofs.ps1 builds every harness a second
    // time with this defined and requires the run to FAIL: an assert(0)
    // that passes means the code above it is unreachable, and every
    // property "proved" there was proved of nothing. A truncated loop or
    // an over-strong assumption produces exactly that, silently.
    //
#ifdef BLORGFS_PROOF_REACHABILITY_PROBE
    __CPROVER_assert(0, "reachability probe: harness body must be reachable");
#endif
}
