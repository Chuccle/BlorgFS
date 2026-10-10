//
// Functional tests for DiskCacheIndex.c, the part of the disk cache that
// decides which slot holds which block: second-miss admission, pinning
// what a read can be served, splitting the rest into fetches, version
// matching, and the clock that picks a victim.
// The cache's I/O (DiskCache.c) is not compiled here; this is the state
// it relies on.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
}

namespace
{

class DiskCacheIndexTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheIndexInitialize(&index, 4));
    }

    void TearDown() override
    {
        BlorgDiskCacheIndexCleanup(&index);
    }

    static DISK_CACHE_KEY Key(ULONG64 File, ULONG64 Block, ULONG64 ModifiedTime = 1000)
    {
        DISK_CACHE_KEY key = {};
        key.File[0] = File;
        key.File[1] = ~File;
        key.Size = 1 << 20;
        key.ModifiedTime = ModifiedTime;
        key.Block = Block;
        return key;
    }

    //
    // Misses the block twice, which admits it, and commits the fill.
    // Returns the slot it went into.
    //
    ULONG Fill(const DISK_CACHE_KEY& Key)
    {
        ULONG slot = DISK_CACHE_NO_SLOT;

        EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &Key, &slot));
        EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &Key, &slot));
        BlorgDiskCacheIndexCommit(&index, slot, TRUE);

        return slot;
    }

    //
    // Whether every block from Key to LastBlock is held. Like a read the
    // cache serves whole, a range found held is unpinned as served, which
    // marks it for the clock; one that is not leaves no mark and no pin.
    //
    BOOLEAN Held(const DISK_CACHE_KEY& Key, ULONG64 LastBlock = MAXULONG64)
    {
        ULONG slots[8];
        const ULONG64 last = (MAXULONG64 == LastBlock) ? Key.Block : LastBlock;
        const ULONG count = C_CAST(ULONG, last - Key.Block + 1);
        const BOOLEAN all = (count == BlorgDiskCacheIndexPinHeld(&index, &Key, last, slots));

        BlorgDiskCacheIndexUnpin(&index, slots, count, all);

        return all;
    }

    DISK_CACHE_INDEX index;
};

TEST_F(DiskCacheIndexTest, ABlockIsAdmittedOnlyOnItsSecondMiss)
{
    const DISK_CACHE_KEY key = Key(1, 0);
    ULONG slot = DISK_CACHE_NO_SLOT;

    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &key, &slot));
    EXPECT_FALSE(Held(key));

    EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &key, &slot));
    EXPECT_FALSE(Held(key));

    BlorgDiskCacheIndexCommit(&index, slot, TRUE);
    EXPECT_TRUE(Held(key));
}

//
// A file read twice is held whole after the second pass, as long as it
// fits: no block of it loses its first miss to another block of the same
// pass. A direct-mapped ghost table lost about a fifth of a file this way,
// on every pass, measured in the guest.
//
TEST_F(DiskCacheIndexTest, EveryBlockMissedOnceIsAdmittedOnItsNextMiss)
{
    DISK_CACHE_INDEX wide;
    ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheIndexInitialize(&wide, 64));

    ULONG slot = DISK_CACHE_NO_SLOT;

    for (ULONG64 block = 0; block < 64; ++block)
    {
        const DISK_CACHE_KEY key = Key(1, block);
        EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&wide, &key, &slot)) << "block " << block;
    }

    for (ULONG64 block = 0; block < 64; ++block)
    {
        const DISK_CACHE_KEY key = Key(1, block);
        ASSERT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&wide, &key, &slot)) << "block " << block;
        BlorgDiskCacheIndexCommit(&wide, slot, TRUE);
    }

    ULONG slots[64];
    const DISK_CACHE_KEY first = Key(1, 0);
    EXPECT_EQ(64u, BlorgDiskCacheIndexPinHeld(&wide, &first, 63, slots));

    BlorgDiskCacheIndexUnpin(&wide, slots, 64, TRUE);

    BlorgDiskCacheIndexCleanup(&wide);
}

TEST_F(DiskCacheIndexTest, ABlockBeingFilledIsNeitherServedNorReservedAgain)
{
    const DISK_CACHE_KEY key = Key(1, 0);
    ULONG slot = DISK_CACHE_NO_SLOT;
    ULONG again = DISK_CACHE_NO_SLOT;

    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &key, &slot));
    EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &key, &slot));

    EXPECT_EQ(DiskCacheHeld, BlorgDiskCacheIndexReserve(&index, &key, &again));
    EXPECT_FALSE(Held(key));

    BlorgDiskCacheIndexCommit(&index, slot, TRUE);
    EXPECT_EQ(DiskCacheHeld, BlorgDiskCacheIndexReserve(&index, &key, &again));
}

TEST_F(DiskCacheIndexTest, AFailedFillLeavesNothingHeldAndStartsAdmissionOver)
{
    const DISK_CACHE_KEY key = Key(1, 0);
    ULONG slot = DISK_CACHE_NO_SLOT;

    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &key, &slot));
    EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &key, &slot));
    BlorgDiskCacheIndexCommit(&index, slot, FALSE);

    EXPECT_FALSE(Held(key));
    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &key, &slot));
}

TEST_F(DiskCacheIndexTest, ABlockIsServedOnlyToTheVersionItWasFilledFor)
{
    Fill(Key(1, 0, 1000));

    EXPECT_TRUE(Held(Key(1, 0, 1000)));
    EXPECT_FALSE(Held(Key(1, 0, 2000)));
    EXPECT_FALSE(Held(Key(2, 0, 1000)));
}

//
// A read is served as far as its blocks are held, so pinning a range pins
// the held blocks and names the holes, rather than refusing the range.
//
TEST_F(DiskCacheIndexTest, OnlyTheHeldBlocksOfARangeArePinned)
{
    const ULONG s0 = Fill(Key(1, 0));
    const ULONG s1 = Fill(Key(1, 1));
    const ULONG s3 = Fill(Key(1, 3));

    ULONG slots[4];
    const DISK_CACHE_KEY first = Key(1, 0);

    ASSERT_EQ(3u, BlorgDiskCacheIndexPinHeld(&index, &first, 3, slots));
    EXPECT_EQ(s0, slots[0]);
    EXPECT_EQ(s1, slots[1]);
    EXPECT_EQ(DISK_CACHE_NO_SLOT, slots[2]);
    EXPECT_EQ(s3, slots[3]);

    for (ULONG i : { 0u, 1u, 3u })
    {
        BlorgDiskCacheIndexUnpin(&index, &slots[i], 1, FALSE);
    }

    //
    // The pins are all gone: with every slot unpinned, two more blocks
    // still find victims.
    //
    Fill(Key(2, 0));
    Fill(Key(2, 1));
    EXPECT_TRUE(Held(Key(2, 0), 1));
}

//
// A block pinned for a read and then fetched instead -- the read failed, or
// the block was swallowed into a fetch -- was not served, so it earns no
// protection from the clock.
//
TEST_F(DiskCacheIndexTest, ABlockPinnedButNotServedIsNotMarked)
{
    for (ULONG64 block = 0; block < 4; ++block)
    {
        Fill(Key(1, block));
    }

    ULONG slot;
    const DISK_CACHE_KEY key = Key(1, 0);

    ASSERT_EQ(1u, BlorgDiskCacheIndexPinHeld(&index, &key, 0, &slot));
    BlorgDiskCacheIndexUnpin(&index, &slot, 1, FALSE);

    Fill(Key(2, 0));

    EXPECT_FALSE(Held(Key(1, 0)));
}

//
// More holes than a read may fetch are joined across the shortest held
// gap, whose blocks are given up and unpinned; the rest stay pinned to be
// served. Every other block held of twelve is six holes, joined into four
// fetches by giving up the first two held gaps (ties go to the first).
//
TEST_F(DiskCacheIndexTest, AReadWithTooManyHolesJoinsTheClosest)
{
    DISK_CACHE_INDEX wide;
    ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheIndexInitialize(&wide, 16));

    ULONG slot = DISK_CACHE_NO_SLOT;

    for (ULONG64 block = 0; block < 12; block += 2)
    {
        const DISK_CACHE_KEY key = Key(1, block);
        ASSERT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&wide, &key, &slot));
        ASSERT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&wide, &key, &slot));
        BlorgDiskCacheIndexCommit(&wide, slot, TRUE);
    }

    ULONG slots[12];
    const DISK_CACHE_KEY first = Key(1, 0);
    const ULONG held = BlorgDiskCacheIndexPinHeld(&wide, &first, 11, slots);
    ASSERT_EQ(6u, held);
    ASSERT_EQ(4, DISK_CACHE_MAX_READ_FETCHES);

    EXPECT_EQ(4u, BlorgDiskCacheIndexPlanRead(&wide, slots, 12, held, DISK_CACHE_MAX_READ_FETCHES));

    for (ULONG i = 0; i < 12; ++i)
    {
        const BOOLEAN served = (0 == i) || (6 == i) || (8 == i) || (10 == i);
        EXPECT_EQ(served, DISK_CACHE_NO_SLOT != slots[i]) << "block " << i;
    }

    //
    // The two given up were unpinned: unpinning the four left frees every
    // slot, so all sixteen can be taken again.
    //
    for (ULONG i : { 0u, 6u, 8u, 10u })
    {
        BlorgDiskCacheIndexUnpin(&wide, &slots[i], 1, FALSE);
    }

    for (ULONG i = 0; i < 16; ++i)
    {
        EXPECT_EQ(0, wide.Slots[i].Pins) << "slot " << i;
    }

    BlorgDiskCacheIndexCleanup(&wide);
}

//
// A read allowed fewer fetches joins its holes further: with the fetch
// limit leaving room for one, the six holes become one run from the first
// to the last, and only the block before it is served.
//
TEST_F(DiskCacheIndexTest, AReadAllowedOneFetchJoinsEveryHole)
{
    DISK_CACHE_INDEX wide;
    ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheIndexInitialize(&wide, 16));

    ULONG slot = DISK_CACHE_NO_SLOT;

    for (ULONG64 block = 0; block < 12; block += 2)
    {
        const DISK_CACHE_KEY key = Key(1, block);
        ASSERT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&wide, &key, &slot));
        ASSERT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&wide, &key, &slot));
        BlorgDiskCacheIndexCommit(&wide, slot, TRUE);
    }

    ULONG slots[12];
    const DISK_CACHE_KEY first = Key(1, 0);
    const ULONG held = BlorgDiskCacheIndexPinHeld(&wide, &first, 11, slots);
    ASSERT_EQ(6u, held);

    EXPECT_EQ(1u, BlorgDiskCacheIndexPlanRead(&wide, slots, 12, held, 1));

    for (ULONG i = 0; i < 12; ++i)
    {
        EXPECT_EQ(0 == i, DISK_CACHE_NO_SLOT != slots[i]) << "block " << i;
    }

    BlorgDiskCacheIndexUnpin(&wide, &slots[0], 1, FALSE);

    for (ULONG i = 0; i < 16; ++i)
    {
        EXPECT_EQ(0, wide.Slots[i].Pins) << "slot " << i;
    }

    BlorgDiskCacheIndexCleanup(&wide);
}

//
// One victim costs the clock DISK_CACHE_CLOCK_REACH slots at most, however
// large the cache and however many slots are marked. Unbounded, a sweep
// after a pass that served every block walked the whole cache under the
// spin lock; with every slot marked, the hand now stops after its reach,
// takes the first slot it passed, and leaves the marks beyond untouched.
//
TEST_F(DiskCacheIndexTest, TheClockLooksNoFurtherThanItsReach)
{
    const ULONG count = 4 * DISK_CACHE_CLOCK_REACH;
    DISK_CACHE_INDEX big;
    ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheIndexInitialize(&big, count));

    ULONG slot = DISK_CACHE_NO_SLOT;

    for (ULONG64 block = 0; block < count; ++block)
    {
        const DISK_CACHE_KEY key = Key(1, block);
        ASSERT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&big, &key, &slot));
        ASSERT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&big, &key, &slot));
        BlorgDiskCacheIndexCommit(&big, slot, TRUE);
    }

    for (ULONG i = 0; i < count; ++i)
    {
        big.Slots[i].Referenced = TRUE;
    }

    const ULONG hand = big.Hand;
    const DISK_CACHE_KEY other = Key(2, 0);

    ASSERT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&big, &other, &slot));
    ASSERT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&big, &other, &slot));
    BlorgDiskCacheIndexCommit(&big, slot, TRUE);

    EXPECT_EQ(hand, slot);
    EXPECT_EQ((hand + DISK_CACHE_CLOCK_REACH) % count, big.Hand);
    EXPECT_TRUE(big.Slots[(hand + DISK_CACHE_CLOCK_REACH) % count].Referenced);

    BlorgDiskCacheIndexCleanup(&big);
}

TEST_F(DiskCacheIndexTest, APinnedBlockIsNeverTheVictim)
{
    const DISK_CACHE_KEY pinned = Key(1, 0);
    ULONG slots[1];

    Fill(pinned);
    Fill(Key(1, 1));
    Fill(Key(1, 2));
    Fill(Key(1, 3));

    ASSERT_EQ(1u, BlorgDiskCacheIndexPinHeld(&index, &pinned, pinned.Block, slots));

    for (ULONG64 block = 10; block < 20; ++block)
    {
        Fill(Key(2, block));
    }

    EXPECT_TRUE(Held(pinned));

    BlorgDiskCacheIndexUnpin(&index, &slots[0], 1, TRUE);
}

TEST_F(DiskCacheIndexTest, ABlockServedSinceTheHandPassedOutlivesOneThatWasNot)
{
    for (ULONG64 block = 0; block < 4; ++block)
    {
        Fill(Key(1, block));
    }

    EXPECT_TRUE(Held(Key(1, 2)));

    Fill(Key(2, 0));

    EXPECT_FALSE(Held(Key(1, 0)));
    EXPECT_TRUE(Held(Key(1, 2)));
}

TEST_F(DiskCacheIndexTest, WithEverySlotPinnedNothingIsAdmitted)
{
    ULONG slots[4];

    for (ULONG64 block = 0; block < 4; ++block)
    {
        Fill(Key(1, block));
    }

    const DISK_CACHE_KEY all = Key(1, 0);
    ASSERT_EQ(4u, BlorgDiskCacheIndexPinHeld(&index, &all, 3, slots));

    const DISK_CACHE_KEY other = Key(2, 0);
    ULONG slot = DISK_CACHE_NO_SLOT;

    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &other, &slot));
    EXPECT_EQ(DiskCacheNoVictim, BlorgDiskCacheIndexReserve(&index, &other, &slot));

    BlorgDiskCacheIndexUnpin(&index, slots, 4, TRUE);

    EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &other, &slot));
    BlorgDiskCacheIndexCommit(&index, slot, TRUE);
}

//
// A read unpins every block it pinned in one call, under one acquisition of
// the index lock, where one call per block took the lock seventeen times
// for a 1 MB hit. The holes a read fetched instead are skipped, every pin
// is dropped, and a read that was served marks each block for the clock.
//
TEST_F(DiskCacheIndexTest, ARangeIsUnpinnedWholeSkippingItsHoles)
{
    const ULONG s0 = Fill(Key(1, 0));
    const ULONG s1 = Fill(Key(1, 1));
    const ULONG s3 = Fill(Key(1, 3));

    const BOOLEAN served[] = { FALSE, TRUE };

    for (BOOLEAN serve : served)
    {
        ULONG slots[4];
        const DISK_CACHE_KEY first = Key(1, 0);

        ASSERT_EQ(3u, BlorgDiskCacheIndexPinHeld(&index, &first, 3, slots));
        ASSERT_EQ(DISK_CACHE_NO_SLOT, slots[2]);

        for (ULONG slot : { s0, s1, s3 })
        {
            index.Slots[slot].Referenced = FALSE;
        }

        BlorgDiskCacheIndexUnpin(&index, slots, 4, serve);

        for (ULONG slot : { s0, s1, s3 })
        {
            EXPECT_EQ(0, index.Slots[slot].Pins) << "slot " << slot;
            EXPECT_EQ(serve, index.Slots[slot].Referenced) << "slot " << slot;
        }
    }
}

}
