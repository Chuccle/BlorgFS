//
// Kernel-behaviour tests for the real Structs.c: FCB/DCB lifetime.
//
// The node table is the driver's answer to "how does a warm open find an
// existing node without taking a global lock, while a close somewhere
// else may be freeing it". The answer is a pin taken under the owning
// bucket's push lock, and a deferred-reap worker that is the single freer
// of published nodes. Its central claim:
//
//   A node returned by BlorgNodeTableLookupPin is never freed while the
//   caller holds the pin.
//
// That is a claim about interleavings, so it is tested with real threads
// against the real code, with the model watching for lock-order
// inversions, push locks taken outside a critical region, paged
// allocation above APC_LEVEL, pool corruption and leaks throughout.
//
// Every fixture asserts quiescence on teardown: a node that survives
// teardown is one the reap worker never freed, which at unload time is a
// leaked paged allocation.
//

#include <gtest/gtest.h>

#include <cstdio>
#include <cwchar>

extern "C" {
#include "..\Driver.h"
}

namespace
{

class NodeTableTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);

        ASSERT_EQ(STATUS_SUCCESS, BlorgNodeTableInit(Volume));

        //
        // Nodes are built through InsertByPath from a root DCB, which is
        // the only way the driver ever builds one. Constructing an FCB
        // directly and freeing it would exercise a shape the driver never
        // produces -- BlorgFreeFileContext unlinks Links unconditionally
        // because every real node was linked into its parent's
        // ChildrenList on the way in, and a hand-made node would fail
        // there for reasons that say nothing about the node table.
        //
        UNICODE_STRING rootName = Path(L"\\");

        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateDCB(&Root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, Volume));
        ASSERT_NE(nullptr, Root);

        //
        // The reap worker takes the VCB resource exclusive for its whole
        // pass, so the volume needs a VCB for the same reason the driver's
        // mount path builds one (Driver.c).
        //
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, Volume, 0));
        ASSERT_NE(nullptr, Vcb);

        GetVolumeDeviceExtension(Volume)->RootDcb = Root;
        GetVolumeDeviceExtension(Volume)->Vcb = Vcb;
    }

    void TearDown() override
    {
        BlorgNodeTableTeardown();

        FreeTree();

        StructsModelDestroyVolume(Volume);

        KmAssertQuiescent("NodeTableTest teardown");
    }

    //
    // Leaf-first teardown of whatever the test left behind, mirroring
    // Driver.c's FreeFileContextTree -- which is static there, so it is not
    // one of the translation units under test.
    //
    void FreeTree()
    {
        while (!IsListEmpty(&Root->ChildrenList))
        {
            PCOMMON_CONTEXT node = CONTAINING_RECORD(Root->ChildrenList.Flink, COMMON_CONTEXT, Links);

            while ((BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(node)) &&
                   !IsListEmpty(&C_CAST(PDCB, node)->ChildrenList))
            {
                node = CONTAINING_RECORD(C_CAST(PDCB, node)->ChildrenList.Flink, COMMON_CONTEXT, Links);
            }

            BlorgFreeFileContext(node, Volume);
        }

        BlorgFreeFileContext(Root, Volume);
        Root = nullptr;

        BlorgFreeFileContext(Vcb, Volume);
        Vcb = nullptr;
    }

    //
    // A node built and published the way a completed cold open leaves one.
    //
    PCOMMON_CONTEXT MakePublishedNode(const wchar_t* path, BOOLEAN IsDirectory = FALSE)
    {
        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = 4096;
        meta.IsDirectory = IsDirectory;

        UNICODE_STRING name = Path(path);
        PCOMMON_CONTEXT node = nullptr;

        EXPECT_EQ(STATUS_SUCCESS, InsertByPath(Root, &name, &meta, Volume, &node));

        if (node)
        {
            BlorgNodeTablePublish(node);
        }

        return node;
    }

    static UNICODE_STRING Path(const wchar_t* path)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(path);
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    PDEVICE_OBJECT Volume = nullptr;
    PDCB Root = nullptr;
    PFCB Vcb = nullptr;
};

///////////////////////////////////////////////////////////////////////////
// Publish and lookup
///////////////////////////////////////////////////////////////////////////

//
// An unpublished node must not be findable. Publication is what makes a
// node visible to the lock-free open path, and a node visible before it
// is fully built is the classic cold-open race.
//
TEST_F(NodeTableTest, UnpublishedNodeIsNotFound)
{
    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    UNICODE_STRING name = Path(L"\\a\\b.txt");
    PCOMMON_CONTEXT node = nullptr;

    ASSERT_EQ(STATUS_SUCCESS, InsertByPath(Root, &name, &meta, Volume, &node));
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(nullptr, BlorgNodeTableLookupPin(&name))
        << "a node that exists in the tree but was never published must not "
           "be reachable through the lock-free path";
}

TEST_F(NodeTableTest, PublishedNodeIsFoundAndPinned)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\a\\b.txt");
    ASSERT_NE(nullptr, node);

    UNICODE_STRING name = Path(L"\\a\\b.txt");

    PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&name);

    ASSERT_EQ(node, found);
    EXPECT_EQ(1, found->PinCount) << "lookup must return the node already pinned";

    BlorgNodeUnpin(found);
}

//
// Lookup is case-insensitive, and the hash must agree with the compare.
// If they disagreed, a node would be published into one bucket and
// searched for in another -- a miss that looks like a cold open every
// time, silently doubling backend traffic rather than failing outright.
//
TEST_F(NodeTableTest, LookupIsCaseInsensitiveAndHashAgreesWithCompare)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\Media\\Big.MKV");
    ASSERT_NE(nullptr, node);

    UNICODE_STRING lower = Path(L"\\media\\big.mkv");

    PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&lower);

    ASSERT_EQ(node, found)
        << "case-folded lookup missed -- hash and compare disagree";

    BlorgNodeUnpin(found);
}

TEST_F(NodeTableTest, MissReturnsNull)
{
    MakePublishedNode(L"\\a\\b.txt");

    UNICODE_STRING other = Path(L"\\a\\c.txt");

    EXPECT_EQ(nullptr, BlorgNodeTableLookupPin(&other));
}

///////////////////////////////////////////////////////////////////////////
// The pin protocol
///////////////////////////////////////////////////////////////////////////

//
// The central claim, single-threaded first: a pinned node survives every
// reap opportunity. The reap worker revalidates both counts under the
// bucket lock, so a nonzero pin must block the free -- and the model's
// guarded pool would report the use-after-free if it did not.
//
TEST_F(NodeTableTest, PinnedNodeSurvivesReap)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\a\\b.txt");
    ASSERT_NE(nullptr, node);

    UNICODE_STRING name = Path(L"\\a\\b.txt");

    PCOMMON_CONTEXT pinned = BlorgNodeTableLookupPin(&name);
    ASSERT_NE(nullptr, pinned);

    //
    // Drive every reap path there is while the pin is held.
    //
    BlorgNodeDeferReap(pinned);
    ShimDrainWorkItems();

    //
    // Still ours: reading through the pointer must be safe. The guarded
    // pool turns a premature free into an immediate, attributable abort
    // rather than a silent read of poisoned memory.
    //
    EXPECT_EQ(1, pinned->PinCount);
    EXPECT_EQ(node, pinned);

    BlorgNodeUnpin(pinned);
    ShimDrainWorkItems();
}

//
// Dropping the last pin on an idle node is what queues it for the worker.
// Without that, a node whose closer's reap attempt lost to this very pin
// would be stranded published forever -- reachable, never freed.
//
TEST_F(NodeTableTest, LastUnpinOfAnIdleNodeQueuesTheReap)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\a\\b.txt");
    ASSERT_NE(nullptr, node);

    UNICODE_STRING name = Path(L"\\a\\b.txt");

    PCOMMON_CONTEXT pinned = BlorgNodeTableLookupPin(&name);
    ASSERT_NE(nullptr, pinned);

    EXPECT_EQ(0, pinned->RefCount) << "no handles were opened, so the node is idle but pinned";

    BlorgNodeUnpin(pinned);

    ShimDrainWorkItems();

    EXPECT_EQ(nullptr, BlorgNodeTableLookupPin(&name))
        << "an idle node should have been reaped once its last pin dropped";
}

//
// A node with an open handle is not idle, so a reap must leave it alone
// however many times it is attempted.
//
TEST_F(NodeTableTest, ReferencedNodeIsNotReaped)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\a\\b.txt");
    ASSERT_NE(nullptr, node);

    InterlockedIncrement64(&(node)->RefCount);

    BlorgNodeDeferReap(node);
    ShimDrainWorkItems();

    UNICODE_STRING name = Path(L"\\a\\b.txt");

    PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&name);
    ASSERT_NE(nullptr, found) << "a node with an open handle was reaped";

    BlorgNodeUnpin(found);

    BlorgNodeDereference(node);
    ShimDrainWorkItems();
}

//
// The deferral claim is a one-shot interlocked gate, so a node cannot be
// pushed onto the reap list twice -- which would corrupt the singly
// linked list it is threaded on.
//
TEST_F(NodeTableTest, RepeatedDeferralDoesNotDoubleQueue)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\a\\b.txt");
    ASSERT_NE(nullptr, node);

    for (int i = 0; i < 8; ++i)
    {
        BlorgNodeDeferReap(node);
    }

    ShimDrainWorkItems();

    UNICODE_STRING name = Path(L"\\a\\b.txt");
    EXPECT_EQ(nullptr, BlorgNodeTableLookupPin(&name));
}

///////////////////////////////////////////////////////////////////////////
// Concurrency -- the claim that matters
///////////////////////////////////////////////////////////////////////////

//
// The contended-node stress. Three roles, matching what the driver
// actually permits to run at once:
//
//   readers    BlorgNodeTableLookupPin / touch / BlorgNodeUnpin, taking
//              only the bucket lock shared -- the lock-free warm-open path
//   publisher  cold open: VCB resource exclusive, InsertByPath, publish
//   reaper     drains the work queue, so NodeReapWorker runs and takes the
//              VCB resource and the bucket lock exclusive
//
// Readers therefore race the reap worker on the bucket lock with the node
// at RefCount 0 -- genuinely reap-eligible while being pinned. An earlier
// version of this test held a handle reference for the whole run, which
// made the node permanently ineligible: it passed against a build whose
// worker did not revalidate PinCount at all. Hence ObservedReaps below --
// a test that never saw a node freed has not tested this.
//
namespace
{
    struct ContendedState
    {
        KM_BARRIER* Barrier;
        const wchar_t* PathText;
        PDCB Root;
        PFCB Vcb;
        PDEVICE_OBJECT Volume;
        volatile long Running;

        volatile long Hits;
        volatile long ObservedReaps;
    };

    UNICODE_STRING MakeName(const wchar_t* text)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(text);
        name.Length = (USHORT)(wcslen(text) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    //
    // Pin, read through the pin, drop it. If a node could be freed while
    // pinned, the read is where it shows: the guarded pool's poison is not
    // a plausible PinCount.
    //
    void ContendedReader(void* Parameter)
    {
        ContendedState* state = (ContendedState*)Parameter;
        UNICODE_STRING name = MakeName(state->PathText);

        KmBarrierWait(state->Barrier);

        while (ReadNoFence(&state->Running))
        {
            PCOMMON_CONTEXT node = BlorgNodeTableLookupPin(&name);

            if (!node)
            {
                KmJitter();
                continue;
            }

            KmJitter();

            volatile LONG pins = node->PinCount;

            if (pins <= 0)
            {
                KmReportViolation(KmViolationLifetime,
                    "pinned node reports PinCount %ld -- freed or reused while pinned", pins);
            }

            BlorgNodeUnpin(node);

            InterlockedIncrement(&state->Hits);
        }
    }

    //
    // Cold open. The driver creates nodes under the VCB resource
    // exclusive, the same resource NodeReapWorker takes, so creation and
    // reaping are mutually exclusive by construction. Doing it any other
    // way here would test an interleaving the driver never allows.
    //
    void ContendedPublisher(void* Parameter)
    {
        ContendedState* state = (ContendedState*)Parameter;
        UNICODE_STRING name = MakeName(state->PathText);

        KmBarrierWait(state->Barrier);

        while (ReadNoFence(&state->Running))
        {
            FsRtlEnterFileSystem();
            ExAcquireResourceExclusiveLite(state->Vcb->Header.Resource, TRUE);

            PCOMMON_CONTEXT existing = BlorgNodeTableLookupPin(&name);

            if (existing)
            {
                BlorgNodeUnpin(existing);
            }
            else
            {
                DIRECTORY_ENTRY_METADATA meta = {};
                meta.Size = 4096;

                PCOMMON_CONTEXT node = nullptr;

                if (NT_SUCCESS(InsertByPath(state->Root, &name, &meta, state->Volume, &node)) && node)
                {
                    BlorgNodeTablePublish(node);

                    //
                    // Finding it absent means the previous incarnation was
                    // reaped, which is the event this test needs to have
                    // happened at all.
                    //
                    InterlockedIncrement(&state->ObservedReaps);
                }
            }

            ExReleaseResourceLite(state->Vcb->Header.Resource);
            FsRtlExitFileSystem();

            KmJitter();
        }
    }

    void ContendedReaper(void* Parameter)
    {
        ContendedState* state = (ContendedState*)Parameter;

        KmBarrierWait(state->Barrier);

        while (ReadNoFence(&state->Running))
        {
            ShimDrainWorkItems();
            KmJitter();
        }
    }
}

TEST_F(NodeTableTest, ConcurrentLookupPinAndReapNeverUseAfterFree)
{
    const wchar_t* path = L"\\media\\contended.bin";

    MakePublishedNode(path);

    const int kReaders = 4;

    ContendedState state = {};
    KM_BARRIER barrier;
    KmInitializeBarrier(&barrier, kReaders + 2);

    state.Barrier = &barrier;
    state.PathText = path;
    state.Root = Root;
    state.Vcb = Vcb;
    state.Volume = Volume;
    state.Running = 1;

    KM_THREAD* readers[kReaders] = {};

    for (int i = 0; i < kReaders; ++i)
    {
        readers[i] = KmStartThread(ContendedReader, &state);
    }

    KM_THREAD* publisher = KmStartThread(ContendedPublisher, &state);
    KM_THREAD* reaper = KmStartThread(ContendedReaper, &state);

    Sleep(250);
    InterlockedExchange(&state.Running, 0);

    for (int i = 0; i < kReaders; ++i)
    {
        KmJoinThread(readers[i]);
    }

    KmJoinThread(publisher);
    KmJoinThread(reaper);

    ShimDrainWorkItems();

    //
    // Coverage assertions, not behaviour ones. The behaviour claim -- no
    // node is freed while pinned -- is enforced by the guarded pool and
    // the PinCount check inside the reader. These two exist so a test that
    // stopped exercising the window fails loudly instead of passing
    // vacuously, which is exactly what the previous version did.
    //
    EXPECT_GT(state.Hits, 0) << "no lookup ever hit -- readers exercised nothing";
    EXPECT_GT(state.ObservedReaps, 0)
        << "no node was ever reaped during the run -- readers never raced the "
           "reap worker, so this test proves nothing about the pin protocol";
}

//
// Distinct paths hash to different buckets, so this exercises the sharding
// itself: several threads publishing and reaping unrelated nodes at once.
// A bucket-index bug (hashing on one thing, indexing on another) shows up
// as a node that cannot be found from the thread that did not create it.
//
//
// Distinct paths hash to different buckets, so this exercises the sharding
// itself: several threads publishing, finding and reaping unrelated nodes
// at once. A bucket-index bug -- hashing on one thing and indexing on
// another, or a reap that unlinks from the wrong bucket -- shows up as a
// node its own publisher cannot find.
//
static const int kShardThreads = 4;
static const int kShardNodesPerThread = 40;

static PDEVICE_OBJECT ShardVolume = nullptr;
static PDCB ShardRoot = nullptr;
static volatile long ShardFound = 0;

static void ShardThread(void* Parameter)
{
    const int index = *(int*)Parameter;

    for (int i = 0; i < kShardNodesPerThread; ++i)
    {
        wchar_t path[64];
        swprintf_s(path, L"\\shard\\t%d_n%d.bin", index, i);

        UNICODE_STRING name;
        name.Buffer = path;
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;

        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = 4096;

        PCOMMON_CONTEXT node = nullptr;

        if (!NT_SUCCESS(InsertByPath(ShardRoot, &name, &meta, ShardVolume, &node)) || !node)
        {
            continue;
        }

        BlorgNodeTablePublish(node);

        KmJitter();

        PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&name);

        if (found)
        {
            InterlockedIncrement(&ShardFound);
            BlorgNodeUnpin(found);
        }

        BlorgNodeDeferReap(node);
    }
}

TEST_F(NodeTableTest, ConcurrentDistinctNodesAcrossBuckets)
{
    ShardVolume = Volume;
    ShardRoot = Root;
    ShardFound = 0;

    int indices[kShardThreads];
    KM_THREAD* threads[kShardThreads] = {};

    for (int i = 0; i < kShardThreads; ++i)
    {
        indices[i] = i;
        threads[i] = KmStartThread(ShardThread, &indices[i]);
    }

    for (int i = 0; i < kShardThreads; ++i)
    {
        KmJoinThread(threads[i]);
    }

    ShimDrainWorkItems();

    EXPECT_EQ(kShardThreads * kShardNodesPerThread, ShardFound)
        << "a node published by one thread was not found by the thread that "
           "published it -- hash and bucket index disagree";
}

///////////////////////////////////////////////////////////////////////////
// Teardown
///////////////////////////////////////////////////////////////////////////

//
// Teardown must drain whatever is left. A node still published at unload
// is a leaked paged allocation, and the reap worker's own work item is
// freed by teardown -- so a kick that slipped past the shutdown latch
// would touch freed memory. The fixture's quiescence assertion covers
// both.
//
TEST_F(NodeTableTest, TeardownDrainsPublishedNodes)
{
    for (int i = 0; i < 16; ++i)
    {
        wchar_t path[64];
        swprintf_s(path, L"\\teardown\\n%d.bin", i);
        MakePublishedNode(path);
    }

    EXPECT_GT(ShimPoolOutstanding(), 0u);

    // TearDown calls BlorgNodeTableTeardown and then asserts quiescence.
}

} // namespace
