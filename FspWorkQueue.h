#pragma once

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

#if (NTDDI_VERSION >= NTDDI_WINTHRESHOLD)
#define IRP_CONTEXT_FLAG_SWAPPED_STACK              0x00100000
#endif

#define IRP_CONTEXT_FLAG_PARENT_BY_CHILD            0x80000000

inline void BlorgSetupIrpContext(PIRP Irp, BOOLEAN Wait)
{
    ULONG_PTR flags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);

    NT_ASSERT(0 == flags);

    if (Wait)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_WAIT);
    }

    //
    //  Set the recursive file system call parameter.  We set it true if
    //  the TopLevelIrp field in the thread local storage is not the current
    //  irp, otherwise we leave it as FALSE.
    //

    if (IoGetTopLevelIrp() != Irp)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL);
    }

    Irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);
}

NTSTATUS FsdPostRequest(IN PIRP Irp, IN PIO_STACK_LOCATION IrpSp);

void PrePostIrp(IN PVOID Context, IN PIRP Irp);

void OplockComplete(PVOID Context, PIRP Irp);

NTSTATUS CreateWorkQueue(void);

void DestroyWorkQueue(void);