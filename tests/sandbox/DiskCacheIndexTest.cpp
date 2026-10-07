//
// Functional tests for DiskCacheIndex.c, the part of the disk cache that
// decides which slot holds which block: second-miss admission, all-or-
// nothing pinning, version matching, and the clock that picks a victim.
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

    BOOLEAN Held(const DISK_CACHE_KEY& Key, ULONG64 LastBlock = MAXULONG64)
    {
        ULONG slots[8];
        const ULONG64 last = (MAXULONG64 == LastBlock) ? Key.Block : LastBlock;

        if (!BlorgDiskCacheIndexPinRange(&index, &Key, last, slots))
        {
            return FALSE;
        }

        for (ULONG64 i = 0; i <= last - Key.Block; ++i)
        {
            BlorgDiskCacheIndexUnpin(&index, slots[i]);
        }

        return TRUE;
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
    EXPECT_TRUE(BlorgDiskCacheIndexPinRange(&wide, &first, 63, slots));

    for (ULONG i = 0; i < 64; ++i)
    {
        BlorgDiskCacheIndexUnpin(&wide, slots[i]);
    }

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

TEST_F(DiskCacheIndexTest, ARangeIsPinnedWholeOrNotAtAll)
{
    Fill(Key(1, 0));
    Fill(Key(1, 1));
    Fill(Key(1, 3));

    EXPECT_TRUE(Held(Key(1, 0), 1));
    EXPECT_FALSE(Held(Key(1, 0), 3));

    //
    // The refused range left no pins behind: with every slot unpinned, two
    // more blocks still find victims.
    //
    Fill(Key(2, 0));
    Fill(Key(2, 1));
    EXPECT_TRUE(Held(Key(2, 0), 1));
}

TEST_F(DiskCacheIndexTest, APinnedBlockIsNeverTheVictim)
{
    const DISK_CACHE_KEY pinned = Key(1, 0);
    ULONG slots[1];

    Fill(pinned);
    Fill(Key(1, 1));
    Fill(Key(1, 2));
    Fill(Key(1, 3));

    ASSERT_TRUE(BlorgDiskCacheIndexPinRange(&index, &pinned, pinned.Block, slots));

    for (ULONG64 block = 10; block < 20; ++block)
    {
        Fill(Key(2, block));
    }

    EXPECT_TRUE(Held(pinned));

    BlorgDiskCacheIndexUnpin(&index, slots[0]);
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
    ASSERT_TRUE(BlorgDiskCacheIndexPinRange(&index, &all, 3, slots));

    const DISK_CACHE_KEY other = Key(2, 0);
    ULONG slot = DISK_CACHE_NO_SLOT;

    EXPECT_EQ(DiskCacheFirstMiss, BlorgDiskCacheIndexReserve(&index, &other, &slot));
    EXPECT_EQ(DiskCacheNoVictim, BlorgDiskCacheIndexReserve(&index, &other, &slot));

    for (ULONG i = 0; i < 4; ++i)
    {
        BlorgDiskCacheIndexUnpin(&index, slots[i]);
    }

    EXPECT_EQ(DiskCacheReserved, BlorgDiskCacheIndexReserve(&index, &other, &slot));
    BlorgDiskCacheIndexCommit(&index, slot, TRUE);
}

}
