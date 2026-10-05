#pragma once

//
// The change-feed follower (ChangeFeed.c): one system thread per mounted
// volume that long-polls the server for what changed and invalidates
// exactly that, so the metadata caches can be trusted far past their TTL.
//

//
// Starts following the feed for VolumeDeviceObject, whose directory-change
// notifications it reports into. Best effort: a failure leaves the caches
// on their short TTL, which is how the driver behaves without a feed. A
// no-op when the ChangeFeed registry value turned it off.
//
VOID BlorgChangeFeedStart(PDEVICE_OBJECT VolumeDeviceObject);

//
// Stops the thread and waits for it, which includes waiting out a poll in
// flight (at most the server's hold time). PASSIVE_LEVEL, before the volume
// it reports into is torn down.
//
VOID BlorgChangeFeedStop(VOID);

//
// Acts on one poll's outcome: a failure takes the feed down, a reset (or a
// batch from a server process other than the one being followed) brings it
// up from nothing, and anything else invalidates what the batch names.
// Takes ownership of Batch. The feed thread calls this for every poll; it
// has external linkage so the sandbox can drive it without a thread.
//
VOID BlorgChangeFeedReceive(NTSTATUS Status, _In_opt_ PCHANGE_BATCH Batch);
