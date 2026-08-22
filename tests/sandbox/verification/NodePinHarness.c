//
// CBMC concurrency proof for the node table pin protocol.
//
// STATUS: NOT ACHIEVABLE WITH CBMC. Kept as the record of why, and
// deliberately absent from tools\Invoke-BlorgProofs.ps1.
//
// The intended claim, from Structs.h, relied on by every warm open:
//
//   A node handed back by BlorgNodeTableLookupPin is not freed while the
//   caller still holds that pin.
//
// The blocker is structural, not a matter of tuning. CBMC's concurrency
// encoding cannot handle a SHARED VARIABLE OF POINTER TYPE: the moment
// one is assigned it abandons the whole program with "pointer handling
// for concurrency is unsound" and checks nothing at all. Verbose tracing
// names the exact store it gives up on:
//
//   Assignment to node..TableLink..Flink [64 bits]
//
// LIST_ENTRY.Flink is how the node table records whether a node is
// published, and the bucket chain is what a lookup walks. So the
// offending construct is the data structure under proof. Modelling it
// away -- which was tried, for both RemoveEntryList and PushEntryList --
// does not help, because the driver assigns Flink directly too, and
// would in any case mean verifying something other than the node table.
//
// A bisection probe pinned this down independently: two threads sharing
// plain counters and a reader/writer lock encode fine, walking and
// inserting into an intrusive list encodes fine, and assigning a
// pointer-typed shared field is the step that fails.
//
// The trap this harness taught, now guarded automatically: with --unwind
// below a loop's trip count CBMC cuts the path, the body becomes
// unreachable, and it reports VERIFICATION SUCCESSFUL for assertions
// that never execute. Three invariants "passed" that way here. The proof
// runner now requires a failing reachability probe before it will call
// anything PROVED.
//
// Exhaustive interleaving checking of this code needs a tool that
// EXECUTES it rather than encoding it symbolically -- a CHESS-style
// systematic scheduler over the gtest kernel model, where pointers are
// concrete and cost nothing.
//

#include "..\..\Structs.c"

//
// How many of the table's 256 buckets this proof initialises.
//
// Only bucket 0 is reachable -- the modelled hash sends every path there
// and the node's stamped TableBucketIndex is 0 -- so the rest exist only
// to keep the initialisation shaped like BlorgNodeTableInit's. The count
// is small on purpose: CBMC's concurrency encoding gives up entirely
// ("pointer handling for concurrency is unsound") once the table presents
// it with 256 distinct lock objects, and refuses to check anything at
// all. Six is enough to keep the loop honest inside the unwind bound
// while staying under that cliff.
//
#define NODE_PIN_PROOF_BUCKETS 4

extern PVOID CbmcWatchedNode;
PLIST_ENTRY CbmcProofBucketList = NULL;
extern volatile int CbmcNodeFreed;

//
// Observable protocol state. PinHeld is raised strictly between the
// lookup returning a node and the matching unpin, which is exactly the
// window the invariant talks about.
//
static volatile int PinHeld = 0;
static volatile int Retired = 0;

static PCOMMON_CONTEXT TheNode = NULL;
static UNICODE_STRING ThePath;
static WCHAR ThePathBuffer[4] = { L'\\', L'a', L'.', L'b' };

//
// The warm-open path. A node found here must stay valid for as long as
// the pin is held -- callers go on to read its size, its share access and
// its oplock through this pointer.
//
static void PinningThread(void)
{
    PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&ThePath);

    if (found)
    {
        PinHeld = 1;

        //
        // The invariant, checked where it matters: the retire path must
        // not have claimed this node while we hold a pin on it.
        //
        __CPROVER_assert(Retired == 0,
            "node retired while a lookup still holds its pin");

        __CPROVER_assert(CbmcNodeFreed == 0,
            "node freed while a lookup still holds its pin");

        PinHeld = 0;

        BlorgNodeUnpin(found);
    }
}

//
// The synchronous reap gate. TRUE means the caller now owns the node and
// will free it, so it must never come back TRUE while a pin is out.
//
static void RetiringThread(void)
{
    if (NodeTableTryRetire(TheNode))
    {
        Retired = 1;

        __CPROVER_assert(PinHeld == 0,
            "retire claimed a node that a lookup was holding pinned");
    }
}

void NodePinHarness(void)
{
    ThePath.Buffer = ThePathBuffer;
    ThePath.Length = sizeof(ThePathBuffer);
    ThePath.MaximumLength = sizeof(ThePathBuffer);

    //
    // Bucket 0 only, and deliberately not in a loop.
    //
    // It is the only reachable bucket: the modelled hash sends every path
    // to 0 and the node's stamped TableBucketIndex is 0, so no other
    // bucket is ever indexed and their state cannot affect the outcome.
    // Initialising all 256 is worse than pointless here -- CBMC's
    // concurrency encoding gives up on that many distinct lock objects
    // ("pointer handling for concurrency is unsound") and refuses to
    // check anything at all. The rest of the table is left as the
    // driver's own zero-initialised static, which is what it is before
    // BlorgNodeTableInit touches it.
    //
    for (ULONG i = 0; i < NODE_PIN_PROOF_BUCKETS; i++)
    {
        ExInitializePushLock(&NodeTable[i].Lock);
        InitializeListHead(&NodeTable[i].List);
    }

    CbmcProofBucketList = &NodeTable[0].List;

    ExInitializePushLock(&NodeReap.Lock);
    NodeReap.List.Next = NULL;
    NodeReap.Queued = 0;
    NodeReap.ShuttingDown = 0;

    //
    // A published node with no handles and no pins: idle, and therefore
    // eligible for retirement the instant nothing is holding it. That is
    // the only state in which the race is reachable at all -- a node with
    // an open handle is never a retire candidate, which is precisely the
    // mistake that made the earlier thread-based test vacuous.
    //
    static COMMON_CONTEXT node;

    node.RefCount = 0;
    node.PinCount = 0;
    node.OnReapList = 0;
    node.TableBucketIndex = 0;
    node.FullPath = ThePath;
    node.Header.NodeTypeCode = BLORGFS_FCB_SIGNATURE;

    TheNode = &node;
    CbmcWatchedNode = &node;

    InsertTailList(&NodeTable[0].List, &node.TableLink);

    __CPROVER_ASYNC_1: PinningThread();

    RetiringThread();

#ifdef BLORGFS_PROOF_REACHABILITY_PROBE
    __CPROVER_assert(0, "reachability probe: harness body must be reachable");
#endif
}
