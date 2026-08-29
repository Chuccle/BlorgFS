#pragma once

//
//  IRP context flags and the FSP work-queue posting/requeue/oplock-completion
//  declarations used to hand IRPs off to worker threads.
//

#define IRP_CONTEXT_FLAG_DISABLE_DIRTY              0x00000001
#define IRP_CONTEXT_FLAG_WAIT                       0x00000002
#define IRP_CONTEXT_FLAG_WRITE_THROUGH              0x00000004
#define IRP_CONTEXT_FLAG_DISABLE_WRITE_THROUGH      0x00000008
#define IRP_CONTEXT_FLAG_RECURSIVE_CALL             0x00000010
#define IRP_CONTEXT_FLAG_DISABLE_POPUPS             0x00000020
#define IRP_CONTEXT_FLAG_DEFERRED_WRITE             0x00000040
#define IRP_CONTEXT_FLAG_VERIFY_READ                0x00000080
#define IRP_CONTEXT_STACK_IO_CONTEXT                0x00000100
#define IRP_CONTEXT_FLAG_IN_FSP                     0x00000200
#define IRP_CONTEXT_FLAG_USER_IO                    0x00000400       // for performance counters
#define IRP_CONTEXT_FLAG_DISABLE_RAISE              0x00000800
#define IRP_CONTEXT_FLAG_OVERRIDE_VERIFY            0x00001000
#define IRP_CONTEXT_FLAG_CLEANUP_BREAKING_OPLOCK    0x00002000
//
//  Set by the async-HTTP completion routine when the network result for a
//  posted IRP is ready, so the second FSP worker pass knows to skip the
//  network call and run only the PASSIVE_LEVEL post-processing.
//
#define IRP_CONTEXT_FLAG_NET_DONE                   0x00004000

//
//  This paging read is Cc's read-ahead rather than a demand fault: nobody
//  is blocked on it. Set in BlorgRead from the top-level IRP, which the
//  cache-manager callbacks stamp with FSRTL_CACHE_TOP_LEVEL_IRP before Cc
//  enters the file system (CacheManager.c) -- the same signal FastFat uses
//  to tell recursion from a genuine top-level request.
//
//  It has to be captured at dispatch and carried, not re-derived later:
//  BlorgIsIrpTopLevel overwrites the top-level IRP a few lines further on,
//  and a posted request re-enters on an FSP worker whose top-level is
//  FSRTL_FSP_TOP_LEVEL_IRP, by which point the distinction is gone.
//
//  The distinction matters because the two are not the same request. A
//  demand read has an application waiting inside CcCopyRead and its
//  latency is what a viewer feels; a read-ahead is speculative and its
//  latency costs nothing unless it delays a demand read behind it.
//
#define IRP_CONTEXT_FLAG_SPECULATIVE_READ           0x00008000

#if (NTDDI_VERSION >= NTDDI_WINTHRESHOLD)
#define IRP_CONTEXT_FLAG_SWAPPED_STACK              0x00100000
#endif

#define IRP_CONTEXT_FLAG_PARENT_BY_CHILD            0x80000000

//
// Initializes the WAIT and RECURSIVE_CALL flags in an IRP's driver-context
// flags word from a clean (zeroed) state.
//
inline VOID BlorgSetupIrpContext(PIRP Irp, BOOLEAN Wait)
{
    ULONG_PTR flags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);

    NT_ASSERT(0 == flags);

    if (Wait)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_WAIT);
    }

    if (IoGetTopLevelIrp() != Irp)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL);
    }

    Irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);
}

NTSTATUS BlorgFsdPostRequest(PIRP Irp, PIO_STACK_LOCATION IrpSp);

NTSTATUS BlorgFsdRequeueRequest(PIRP Irp);

NTSTATUS BlorgPrePostIrp(PVOID Context, PIRP Irp);

VOID BlorgOplockPrePostIrp(PVOID Context, PIRP Irp);

VOID BlorgOplockComplete(PVOID Context, PIRP Irp);

NTSTATUS BlorgCreateWorkQueue(VOID);

VOID BlorgDestroyWorkQueue(VOID);
