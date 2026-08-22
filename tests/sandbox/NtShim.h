#pragma once

//
// NT/WSK substitute backed by the rule model in KernelModel.h.
//
// This is a superset of what the first sandbox needed, because its point
// is to compile the real Socket.c -- the file that owns the per-operation
// watchdog, the timeout DPC, and the refcount protocol that arbitrates
// between a DPC and a completion racing to free the same context. None of
// that is testable against a faked socket layer: the code under test IS
// the socket layer.
//
// So IRPs, MDLs, lookaside lists, spin locks, timers and DPCs are all
// modelled rather than stubbed, and the WSK provider below is a scriptable
// peer that completes IRPs the way WSK does -- inline, deferred, or never
// (which is what makes a watchdog fire).
//

#include "KernelModel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef C_CAST
#define C_CAST(T, expr) ((T)(expr))
#endif

///////////////////////////////////////////////////////////////////////////
// Base types and status codes
///////////////////////////////////////////////////////////////////////////

typedef long NTSTATUS;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (C_CAST(NTSTATUS, Status) >= 0)
#define NT_ERROR(Status)   ((C_CAST(ULONG, Status) >> 30) == 3)

#define ARGUMENT_PRESENT(p) ((p) != NULL)

//
// Builds a UNICODE_STRING over a string literal without a runtime call.
// The cast is what lets it initialise a const-qualified local, which is
// how the driver uses it.
//
#define RTL_CONSTANT_STRING(s)     { sizeof(s) - sizeof((s)[0]), sizeof(s), (PWSTR)(s) }

#define FlagOn(F, SF)        ((F) & (SF))
#define BooleanFlagOn(F, SF) ((BOOLEAN)(((F) & (SF)) != 0))
#define SetFlag(F, SF)       ((F) |= (SF))
#define ClearFlag(F, SF)     ((F) &= ~(SF))

//
// A failed NT_ASSERT is a bug the sandbox should stop on, not a message
// it should print and continue past. Routing it through
// KmReportViolation puts it in the same place, with the same
// attribution, as every other rule the model enforces.
//
#define NT_ASSERT(Expr) ((Expr) ? (void)0 : KmReportViolation(KmViolationLifetime, "NT_ASSERT failed: %s", #Expr))
#define ASSERT(Expr)    NT_ASSERT(Expr)
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   C_CAST(NTSTATUS, 0x00000000L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         C_CAST(NTSTATUS, 0xC000000DL)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING                   C_CAST(NTSTATUS, 0x00000103L)
#endif
#define STATUS_BUFFER_TOO_SMALL          C_CAST(NTSTATUS, 0xC0000023L)
#define STATUS_INSUFFICIENT_RESOURCES    C_CAST(NTSTATUS, 0xC000009AL)
#define STATUS_OBJECT_NAME_NOT_FOUND     C_CAST(NTSTATUS, 0xC0000034L)
#define STATUS_CONNECTION_DISCONNECTED   C_CAST(NTSTATUS, 0xC000020CL)
#define STATUS_CONNECTION_RESET          C_CAST(NTSTATUS, 0xC000020DL)
#define STATUS_IO_TIMEOUT                C_CAST(NTSTATUS, 0xC00000B5L)
#define STATUS_NOT_FOUND                 C_CAST(NTSTATUS, 0xC0000225L)
#define STATUS_UNSUCCESSFUL              C_CAST(NTSTATUS, 0xC0000001L)
#define STATUS_NAME_TOO_LONG             C_CAST(NTSTATUS, 0xC0000106L)
#define STATUS_BUFFER_OVERFLOW           C_CAST(NTSTATUS, 0x80000005L)
#define STATUS_CANCELLED                 C_CAST(NTSTATUS, 0xC0000120L)
#define STATUS_MORE_PROCESSING_REQUIRED  C_CAST(NTSTATUS, 0xC0000016L)
#define STATUS_DEVICE_NOT_READY          C_CAST(NTSTATUS, 0xC00000A3L)
#define STATUS_REVISION_MISMATCH         C_CAST(NTSTATUS, 0xC0000059L)
#define STATUS_CONNECTION_REFUSED        C_CAST(NTSTATUS, 0xC0000236L)
#define STATUS_INVALID_NETWORK_RESPONSE  C_CAST(NTSTATUS, 0xC00000C3L)

typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef char*          PCHAR;
typedef void*          PVOID;
typedef unsigned __int64 ULONG64;
typedef __int64          LONG64;

#ifndef VOID
#define VOID void
#endif

#ifndef MAXUSHORT
#define MAXUSHORT 0xffff
#endif
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef RTL_NUMBER_OF
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define PASSIVE_LEVEL  0
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2

typedef short CSHORT;

typedef ULONG DEVICE_TYPE;

typedef UCHAR KIRQL;
typedef KIRQL* PKIRQL;

typedef struct _UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, * PUNICODE_STRING;

typedef const UNICODE_STRING* PCUNICODE_STRING;
typedef const WCHAR* PCWCH;

typedef struct _UTF8_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} UTF8_STRING, * PUTF8_STRING;

typedef struct _STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} STRING, * PSTRING;

#ifndef RtlCopyMemory
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#endif
#ifndef RtlMoveMemory
#define RtlMoveMemory(d, s, n) memmove((d), (s), (n))
#endif
#ifndef RtlZeroMemory
#define RtlZeroMemory(d, n)    memset((d), 0, (n))
#endif
#define RtlSecureZeroMemory(d, n) SecureZeroMemory((d), (n))

#define ReadNoFence(p)        (*(volatile LONG*)(p))
#define ReadNoFence64(p)      (*(volatile LONG64*)(p))
#define ReadPointerAcquire(p) (*(PVOID volatile*)(p))

///////////////////////////////////////////////////////////////////////////
// IRQL, routed through the model so every rule is enforced
///////////////////////////////////////////////////////////////////////////

#define KeGetCurrentIrql()   KmGetIrql()
#define KeRaiseIrql(n, old)  (*(old) = KmRaiseIrql(n))
#define KeLowerIrql(old)     KmLowerIrql(old)

VOID KeEnterCriticalRegion(VOID);
VOID KeLeaveCriticalRegion(VOID);

///////////////////////////////////////////////////////////////////////////
// Pool
///////////////////////////////////////////////////////////////////////////

typedef enum _POOL_TYPE
{
    NonPagedPool = 0,
    PagedPool = 1,
    NonPagedPoolNx = 512
} POOL_TYPE;

#define POOL_NX_ALLOCATION 0x1

PVOID ExAllocatePoolZero(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
PVOID ExAllocatePoolUninitialized(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
VOID  ExFreePool(PVOID P);

//
// ReallocateBufferUninitialized is a real inline in Util.h and comes
// from there; the sandbox does not reimplement it. Its exact contract
// (returns the ORIGINAL pointer on failure, always a different one on
// success) is what Client.c detects failure by, so a second
// implementation here would be a place for the two to disagree.
//

SIZE_T ShimPoolOutstanding(VOID);

//
// Fails the Nth allocation from now (-1 disables). The driver's
// allocation-failure branches are otherwise unreachable, which makes them
// the least-tested and most dangerous code it has.
//
VOID ShimPoolFailAt(LONG Index);

//
// Raises *Flag the moment Block is freed, whichever allocator frees it.
// This is how a test observes "the node was freed" at the instant it
// happens rather than inferring it afterwards -- which matters when the
// question is whether the free raced something else.
//
VOID ShimWatchFree(PVOID Block, volatile LONG* Flag);

//
// The kernel's lookaside lists nest their common fields in a GENERAL_
// LOOKASIDE named L, and the driver reads the entry size back out through
// it (Structs.c: RtlZeroMemory(node, list->L.Size)). Flattening that here
// would compile only because the sandbox never used the field -- and would
// then fail the moment a driver change did.
//
typedef struct _GENERAL_LOOKASIDE
{
    ULONG Size;
    ULONG Tag;
    ULONG Depth;
} GENERAL_LOOKASIDE, * PGENERAL_LOOKASIDE;

typedef struct _NPAGED_LOOKASIDE_LIST
{
    GENERAL_LOOKASIDE L;
    LONG Outstanding;
} NPAGED_LOOKASIDE_LIST, * PNPAGED_LOOKASIDE_LIST;

VOID ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PVOID Allocate, PVOID Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth);
VOID ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside);
PVOID ExAllocateFromNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside);
VOID ExFreeToNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry);

typedef enum _MM_SYSTEMSIZE { MmSmallSystem = 0, MmMediumSystem = 1, MmLargeSystem = 2 } MM_SYSTEMSIZE;

MM_SYSTEMSIZE MmQuerySystemSize(VOID);

///////////////////////////////////////////////////////////////////////////
// Lists
///////////////////////////////////////////////////////////////////////////

typedef struct _LIST_ENTRY_SHIM
{
    struct _LIST_ENTRY_SHIM* Flink;
    struct _LIST_ENTRY_SHIM* Blink;
} LIST_ENTRY_SHIM, * PLIST_ENTRY_SHIM;

#define LIST_ENTRY  LIST_ENTRY_SHIM
#define PLIST_ENTRY PLIST_ENTRY_SHIM

#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) \
    ((type*)((char*)(address) - (SIZE_T)(&((type*)0)->field)))
#endif

static __inline void InitializeListHead(PLIST_ENTRY Head)
{
    Head->Flink = Head;
    Head->Blink = Head;
}

static __inline BOOLEAN IsListEmpty(const LIST_ENTRY* Head)
{
    return (BOOLEAN)(Head->Flink == Head);
}

static __inline void InsertHeadList(PLIST_ENTRY Head, PLIST_ENTRY Entry)
{
    PLIST_ENTRY first = Head->Flink;
    Entry->Flink = first;
    Entry->Blink = Head;
    first->Blink = Entry;
    Head->Flink = Entry;
}

static __inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY Head)
{
    PLIST_ENTRY entry = Head->Flink;
    PLIST_ENTRY next = entry->Flink;
    Head->Flink = next;
    next->Blink = Head;
    return entry;
}

///////////////////////////////////////////////////////////////////////////
// Spin locks -- the model's, so order and recursion are checked
///////////////////////////////////////////////////////////////////////////

//
// winnt.h already declares KSPIN_LOCK as a ULONG_PTR, so the name is
// aliased with a macro rather than typedef'd: macro substitution happens
// before the stale typedef is consulted, so every use site in Socket.h
// and Socket.c resolves to the model's lock without either header
// needing to know.
//
#define KSPIN_LOCK  KM_LOCK
#define PKSPIN_LOCK KM_LOCK*

#define KeInitializeSpinLock(lock)          KmInitializeLock((lock), #lock)
#define KeAcquireSpinLock(lock, oldIrql)    (*(oldIrql) = KmAcquireLock(lock))
#define KeReleaseSpinLock(lock, oldIrql)    KmReleaseLock((lock), (oldIrql))

///////////////////////////////////////////////////////////////////////////
// Push locks and resources
///////////////////////////////////////////////////////////////////////////

//
// Push locks and ERESOURCEs are the node table's synchronisation: a
// per-bucket push lock taken shared for lookup and exclusive for
// publish/retire, and the VCB resource the reap worker holds across a
// batch. Both are modelled with real shared/exclusive semantics and both
// participate in the lock-order graph, because the interesting failure in
// that design is not a missing lock -- it is taking two in the wrong
// order, or taking a push lock outside a critical region.
//
typedef struct _EX_PUSH_LOCK
{
    SRWLOCK Lock;
    LONG Initialized;
    int Id;
    const char* Name;

    //
    // Exclusive owner, for detecting recursive acquisition. Shared
    // holders are not tracked individually: recursive *shared*
    // acquisition is legal, and tracking every reader would need a set
    // rather than a word.
    //
    unsigned long ExclusiveOwner;

    //
    // Lock state under systematic exploration only: 0 free, -1 exclusive,
    // n > 0 shared holders. Unused when real threads run against the
    // SRWLOCK above.
    //
    int SchedState;
} EX_PUSH_LOCK, * PEX_PUSH_LOCK;

VOID ExInitializePushLock(PEX_PUSH_LOCK Lock);
VOID ExAcquirePushLockExclusive(PEX_PUSH_LOCK Lock);
VOID ExReleasePushLockExclusive(PEX_PUSH_LOCK Lock);
VOID ExAcquirePushLockShared(PEX_PUSH_LOCK Lock);
VOID ExReleasePushLockShared(PEX_PUSH_LOCK Lock);

typedef struct _ERESOURCE
{
    SRWLOCK Lock;
    LONG Initialized;
    int Id;
    unsigned long ExclusiveOwner;

    //
    // Lock state under systematic exploration only, same shape and same
    // reason as EX_PUSH_LOCK.SchedState: an OS-blocking SRWLOCK acquire
    // deadlocks instantly when the holder is a suspended thread and only
    // the scheduler can wake it.
    //
    int SchedState;
} ERESOURCE, * PERESOURCE;

NTSTATUS ExInitializeResourceLite(PERESOURCE Resource);
NTSTATUS ExDeleteResourceLite(PERESOURCE Resource);
BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait);
BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait);
VOID ExReleaseResourceLite(PERESOURCE Resource);

typedef struct _FAST_MUTEX { SRWLOCK Lock; } FAST_MUTEX, * PFAST_MUTEX;

VOID ExInitializeFastMutex(PFAST_MUTEX Mutex);

//
// Filesystem entry/exit disables normal kernel APCs. Modelled as a
// critical-region enter/leave so a push lock taken inside one is legal
// and one taken outside is not.
//
VOID FsRtlEnterFileSystem(VOID);
VOID FsRtlExitFileSystem(VOID);

///////////////////////////////////////////////////////////////////////////
// Singly-linked lists
///////////////////////////////////////////////////////////////////////////

typedef struct _SINGLE_LIST_ENTRY_SHIM
{
    struct _SINGLE_LIST_ENTRY_SHIM* Next;
} SINGLE_LIST_ENTRY_SHIM, * PSINGLE_LIST_ENTRY_SHIM;

#define SINGLE_LIST_ENTRY  SINGLE_LIST_ENTRY_SHIM
#define PSINGLE_LIST_ENTRY PSINGLE_LIST_ENTRY_SHIM


static __inline void PushEntryList(PSINGLE_LIST_ENTRY Head, PSINGLE_LIST_ENTRY Entry)
{
    Entry->Next = Head->Next;
    Head->Next = Entry;
}

static __inline PSINGLE_LIST_ENTRY PopEntryList(PSINGLE_LIST_ENTRY Head)
{
    PSINGLE_LIST_ENTRY first = Head->Next;

    if (first)
    {
        Head->Next = first->Next;
    }

    return first;
}

static __inline void InsertTailList(PLIST_ENTRY Head, PLIST_ENTRY Entry)
{
    PLIST_ENTRY last = Head->Blink;
    Entry->Flink = Head;
    Entry->Blink = last;
    last->Flink = Entry;
    Head->Blink = Entry;
}

//
// Unlinking twice is how a node ends up on a bucket list it was already
// removed from, so the model poisons the links and reports a second
// removal rather than silently corrupting the list.
//

static __inline void RemoveEntryList(PLIST_ENTRY Entry)
{
    PLIST_ENTRY flink = Entry->Flink;
    PLIST_ENTRY blink = Entry->Blink;

    if (!flink || !blink)
    {
        KmReportViolation(KmViolationLifetime, "RemoveEntryList on an unlinked entry");
        return;
    }

    blink->Flink = flink;
    flink->Blink = blink;

    Entry->Flink = NULL;
    Entry->Blink = NULL;
}

///////////////////////////////////////////////////////////////////////////
// Paged lookaside lists
///////////////////////////////////////////////////////////////////////////

typedef NPAGED_LOOKASIDE_LIST PAGED_LOOKASIDE_LIST, * PPAGED_LOOKASIDE_LIST;

VOID ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PVOID Allocate, PVOID Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth);
VOID ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside);
PVOID ExAllocateFromPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside);
VOID ExFreeToPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry);

///////////////////////////////////////////////////////////////////////////
// Unicode helpers
///////////////////////////////////////////////////////////////////////////

#define HASH_STRING_ALGORITHM_DEFAULT 0

NTSTATUS RtlHashUnicodeString(const UNICODE_STRING* String, BOOLEAN CaseInSensitive, ULONG Algorithm, PULONG Value);
WCHAR RtlUpcaseUnicodeChar(WCHAR Source);
BOOLEAN RtlEqualUnicodeString(const UNICODE_STRING* String1, const UNICODE_STRING* String2, BOOLEAN CaseInSensitive);
VOID RtlFreeUnicodeString(PUNICODE_STRING String);


#define ReadAcquire(p)        (*(volatile LONG*)(p))
#define ReadPointerNoFence(p) (*(PVOID volatile*)(p))

///////////////////////////////////////////////////////////////////////////
// Timers and DPCs -- the model's, on the virtual clock
///////////////////////////////////////////////////////////////////////////

typedef KM_DPC KDPC, * PKDPC;
typedef KM_TIMER KTIMER, * PKTIMER;

typedef VOID KDEFERRED_ROUTINE(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);
typedef KDEFERRED_ROUTINE* PKDEFERRED_ROUTINE;

VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context);
VOID KeInitializeTimer(PKTIMER Timer);
BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
BOOLEAN KeCancelTimer(PKTIMER Timer);

///////////////////////////////////////////////////////////////////////////
// Events
///////////////////////////////////////////////////////////////////////////

typedef struct _KEVENT
{
    HANDLE Handle;
} KEVENT, * PKEVENT;

typedef enum _EVENT_TYPE { NotificationEvent = 0, SynchronizationEvent = 1 } EVENT_TYPE;
typedef enum _KWAIT_REASON { Executive = 0 } KWAIT_REASON;
typedef enum _KPROCESSOR_MODE { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;

VOID KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON Reason, KPROCESSOR_MODE Mode, BOOLEAN Alertable, PVOID Timeout);

//
// Declared here rather than with the other Rtl/Ke helpers because it
// needs KPROCESSOR_MODE, which the events block above defines.
//
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);

#define IO_NO_INCREMENT   0
#define EVENT_INCREMENT   1
#define IO_DISK_INCREMENT 1

///////////////////////////////////////////////////////////////////////////
// MDLs
///////////////////////////////////////////////////////////////////////////

typedef struct _MDL
{
    PVOID  Base;
    SIZE_T Length;
    BOOLEAN Locked;
    BOOLEAN BuiltForNonPaged;
} MDL, * PMDL;

#define NormalPagePriority  16
#define MdlMappingNoExecute 0x40000000

typedef enum _LOCK_OPERATION
{
    IoReadAccess,
    IoWriteAccess,
    IoModifyAccess
} LOCK_OPERATION;

PMDL IoAllocateMdl(PVOID Base, ULONG Length, BOOLEAN Secondary, BOOLEAN ChargeQuota, PVOID Irp);
VOID IoFreeMdl(PMDL Mdl);
VOID MmProbeAndLockPages(PMDL Mdl, KPROCESSOR_MODE AccessMode, LOCK_OPERATION Operation);
VOID MmUnlockPages(PMDL Mdl);
VOID MmBuildMdlForNonPagedPool(PMDL Mdl);
PVOID MmGetSystemAddressForMdlSafe(PMDL Mdl, ULONG Priority);

VOID ShimFailNextMdlMapping(VOID);

//
// MDLs built by a test rather than by the driver: the paging-read path
// arrives with one already describing the caller's buffer, so a test has
// to be able to hand one in.
//
PMDL ShimCreateMdl(PVOID Base, SIZE_T Length);
VOID ShimFreeMdl(PMDL Mdl);

///////////////////////////////////////////////////////////////////////////
// Kernel stack budget
///////////////////////////////////////////////////////////////////////////

//
// Client.c decides whether to continue a completion chain inline or bounce
// it to a worker by asking how much kernel stack is left, and expands the
// stack when a deep chain needs it. Both halves are modelled: the budget
// is a number a test sets, and expansion raises it for the duration of the
// callout, so a test can drive the client down either branch deliberately
// instead of hoping the host stack happens to be shallow.
//
typedef VOID (*PEXPAND_STACK_CALLOUT)(PVOID Parameter);

SIZE_T IoGetRemainingStackSize(VOID);

NTSTATUS KeExpandKernelStackAndCalloutEx(
    PEXPAND_STACK_CALLOUT Callout,
    PVOID Parameter,
    SIZE_T Size,
    BOOLEAN Wait,
    PVOID Context);

VOID ShimSetRemainingStack(SIZE_T Bytes);
VOID ShimFailNextStackExpansion(VOID);

///////////////////////////////////////////////////////////////////////////
// IRPs
///////////////////////////////////////////////////////////////////////////

//
// Object types the driver only compares against or passes through; it
// never dereferences one.
//
typedef struct _OBJECT_TYPE OBJECT_TYPE, * POBJECT_TYPE;
//
// ntifs.h declares this as a pointer TO a POBJECT_TYPE, and drivers
// dereference it at the call site -- ObReferenceObjectByHandle(..,
// *PsThreadType, ..). Declaring it one level shallower compiles until
// something actually uses it.
//
extern POBJECT_TYPE* PsThreadType;

typedef struct _ETHREAD ETHREAD, * PETHREAD;
typedef struct _EPROCESS EPROCESS, * PEPROCESS;

//
// Identity only. The driver compares these against stored values -- "is
// this the thread Cc handed us for lazy write?" -- and never dereferences
// them, so a distinct value per OS thread is the whole contract.
//
PETHREAD PsGetCurrentThread(VOID);
PEPROCESS PsGetCurrentProcess(VOID);

typedef struct _IRP IRP, * PIRP;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT, * PDEVICE_OBJECT;

//
// The I/O objects the driver's dispatch surface names. Driver.h declares
// every BlorgXxx dispatch routine, so these must exist even in targets
// that never run one -- the sandbox exercises the async client, the
// prefetch ring and the node table, not IRP_MJ dispatch.
//
// Only the fields the driver actually reads are here, and DEVICE_OBJECT
// is real rather than opaque because BlorgGetVolumeDeviceExtension reaches
// through it to reach the lookaside lists every node is allocated from.
//
struct _DEVICE_OBJECT
{
    PVOID DeviceExtension;
    ULONG Flags;
    ULONG SectorSize;
    DEVICE_TYPE DeviceType;      // reported through FileFsDeviceInformation
    ULONG Characteristics;       // ditto -- read-only, remote, and so on
    struct _VPB* Vpb;
};

#define MAXIMUM_VOLUME_LABEL_LENGTH (32 * sizeof(WCHAR))

typedef struct _VPB
{
    CSHORT Type;
    CSHORT Size;
    USHORT Flags;
    USHORT VolumeLabelLength;    // in bytes, not characters
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT RealDevice;
    ULONG SerialNumber;
    ULONG ReferenceCount;
    WCHAR VolumeLabel[MAXIMUM_VOLUME_LABEL_LENGTH / sizeof(WCHAR)];
} VPB, * PVPB;

typedef struct _FILE_OBJECT
{
    PDEVICE_OBJECT DeviceObject;
    PVPB Vpb;
    PVOID FsContext;
    PVOID FsContext2;
    struct _SECTION_OBJECT_POINTERS* SectionObjectPointer;
    PVOID PrivateCacheMap;
    UNICODE_STRING FileName;
    LARGE_INTEGER CurrentByteOffset;
    ULONG Flags;
    struct _FILE_OBJECT* RelatedFileObject;
} FILE_OBJECT, * PFILE_OBJECT;

struct _IO_STACK_LOCATION;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, * PIO_STACK_LOCATION;

//
// Top-level IRP is per-thread state in the kernel, and the recursion
// guards in Util.h/FspWorkQueue.h depend on that -- a shared global would
// make two sandbox threads see each other's re-entrancy.
//
PIRP IoGetTopLevelIrp(VOID);
VOID IoSetTopLevelIrp(PIRP Irp);

typedef BOOLEAN CACHE_ACQUIRE_ROUTINE(PVOID Context, BOOLEAN Wait);
typedef VOID CACHE_RELEASE_ROUTINE(PVOID Context);

typedef struct _CACHE_MANAGER_CALLBACKS
{
    CACHE_ACQUIRE_ROUTINE* AcquireForLazyWrite;
    CACHE_RELEASE_ROUTINE* ReleaseFromLazyWrite;
    CACHE_ACQUIRE_ROUTINE* AcquireForReadAhead;
    CACHE_RELEASE_ROUTINE* ReleaseFromReadAhead;
} CACHE_MANAGER_CALLBACKS, * PCACHE_MANAGER_CALLBACKS;

///////////////////////////////////////////////////////////////////////////
// Cancel-safe queue
///////////////////////////////////////////////////////////////////////////

//
// The FSP work queue parks IRPs on an IO_CSQ. The one behaviour that
// matters to anything testing it is the one that is easy to forget:
// IoCsqInsertIrp marks the IRP pending itself, so a caller that also
// calls IoMarkIrpPending is not being careful, it is double-marking. The
// model therefore does the marking, exactly as the kernel does.
//
typedef struct _IO_CSQ IO_CSQ, * PIO_CSQ;
typedef struct _IO_CSQ_IRP_CONTEXT IO_CSQ_IRP_CONTEXT, * PIO_CSQ_IRP_CONTEXT;

typedef VOID IO_CSQ_INSERT_IRP(PIO_CSQ Csq, PIRP Irp);
typedef VOID IO_CSQ_REMOVE_IRP(PIO_CSQ Csq, PIRP Irp);
typedef PIRP IO_CSQ_PEEK_NEXT_IRP(PIO_CSQ Csq, PIRP Irp, PVOID PeekContext);
typedef VOID IO_CSQ_ACQUIRE_LOCK(PIO_CSQ Csq, PKIRQL Irql);
typedef VOID IO_CSQ_RELEASE_LOCK(PIO_CSQ Csq, KIRQL Irql);
typedef VOID IO_CSQ_COMPLETE_CANCELED_IRP(PIO_CSQ Csq, PIRP Irp);

struct _IO_CSQ
{
    IO_CSQ_INSERT_IRP* CsqInsertIrp;
    IO_CSQ_REMOVE_IRP* CsqRemoveIrp;
    IO_CSQ_PEEK_NEXT_IRP* CsqPeekNextIrp;
    IO_CSQ_ACQUIRE_LOCK* CsqAcquireLock;
    IO_CSQ_RELEASE_LOCK* CsqReleaseLock;
    IO_CSQ_COMPLETE_CANCELED_IRP* CsqCompleteCanceledIrp;
};

NTSTATUS IoCsqInitialize(
    PIO_CSQ Csq,
    IO_CSQ_INSERT_IRP* CsqInsertIrp,
    IO_CSQ_REMOVE_IRP* CsqRemoveIrp,
    IO_CSQ_PEEK_NEXT_IRP* CsqPeekNextIrp,
    IO_CSQ_ACQUIRE_LOCK* CsqAcquireLock,
    IO_CSQ_RELEASE_LOCK* CsqReleaseLock,
    IO_CSQ_COMPLETE_CANCELED_IRP* CsqCompleteCanceledIrp);

VOID IoCsqInsertIrp(PIO_CSQ Csq, PIRP Irp, PIO_CSQ_IRP_CONTEXT Context);
PIRP IoCsqRemoveNextIrp(PIO_CSQ Csq, PVOID PeekContext);

//
// Top-level IRP sentinel values. FSRTL_CACHE_TOP_LEVEL_IRP is what a
// filesystem sets while it is inside the Cache Manager, so a recursive
// entry can tell it is being re-entered by Cc rather than by a new
// request -- which is exactly what the lazy-write and read-ahead
// callbacks in CacheManager.c key off.
//
#define FSRTL_FSP_TOP_LEVEL_IRP      C_CAST(PIRP, 0x01)
#define FSRTL_CACHE_TOP_LEVEL_IRP    C_CAST(PIRP, 0x02)
#define FSRTL_MOD_WRITE_TOP_LEVEL_IRP C_CAST(PIRP, 0x03)
#define FSRTL_FAST_IO_TOP_LEVEL_IRP  C_CAST(PIRP, 0x04)
#define FSRTL_MAX_TOP_LEVEL_IRP_FLAG C_CAST(PIRP, 0x05)

typedef NTSTATUS DRIVER_DISPATCH(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef DRIVER_DISPATCH* PDRIVER_DISPATCH;

typedef VOID DRIVER_UNLOAD(struct _DRIVER_OBJECT* DriverObject);
typedef DRIVER_UNLOAD* PDRIVER_UNLOAD;

#define IRP_MJ_MAXIMUM_FUNCTION 0x1b

typedef struct _DRIVER_OBJECT
{
    PDRIVER_UNLOAD DriverUnload;
    PVOID FastIoDispatch;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
} DRIVER_OBJECT, * PDRIVER_OBJECT;

#define IRP_NOCACHE          0x00000001
#define IRP_PAGING_IO        0x00000002
#define IRP_SYNCHRONOUS_API  0x00000004
#define IRP_BUFFERED_IO      0x00000010
#define IRP_INPUT_OPERATION  0x00000040

typedef NTSTATUS IO_COMPLETION_ROUTINE(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);
typedef IO_COMPLETION_ROUTINE* PIO_COMPLETION_ROUTINE;

typedef struct _IO_STATUS_BLOCK
{
    NTSTATUS Status;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, * PIO_STATUS_BLOCK;

struct _IRP
{
    IO_STATUS_BLOCK IoStatus;

    //
    // The buffer the request targets. Paging reads arrive with this
    // already set by MM, and the prefetch ring copies into it for both a
    // hit and a parked delivery.
    //
    PMDL MdlAddress;

    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID CompletionContext;

    BOOLEAN Cancel;
    BOOLEAN Completed;

    //
    // Counts every completion attempt, not just the first, so a test can
    // assert "completed exactly once" rather than only "completed".
    //
    ULONG CompletionCount;
    BOOLEAN Freed;

    //
    // Set by IoMarkIrpPending. A dispatch routine that returns
    // STATUS_PENDING must have marked the IRP first, or pending status
    // never propagates to a filter layered above and its completion
    // routine mishandles the request. Modelling the flag lets a test
    // assert the contract instead of trusting it.
    //
    BOOLEAN PendingReturned;

    //
    // Set while the owning transport still holds the IRP. IoCancelIrp on
    // an outstanding IRP is what a watchdog does; on a completed one it
    // must be harmless, and the model asserts the difference rather than
    // letting a use-after-free look like success.
    //
    BOOLEAN Outstanding;

    PVOID TransportContext;

    //
    // The fields the driver's own IRP helpers reach for. Tail.Overlay
    // .DriverContext[0] is where BlorgFS stores its per-request flag word
    // (Util.h), so it has to be a real, distinct slot rather than a
    // convenient alias for something else.
    //
    //
    // Buffered I/O hands the driver a system copy of the caller's buffer
    // here; the union has other arms in the kernel that BlorgFS never uses.
    //
    union
    {
        PVOID SystemBuffer;
    } AssociatedIrp;

    ULONG Flags;
    PVOID UserBuffer;
    KPROCESSOR_MODE RequestorMode;

    //
    // One stack location, not a stack of them: BlorgFS never calls down to
    // a lower driver, so the next-lower location is never used.
    //
    struct _IO_STACK_LOCATION* StackLocation;

    struct
    {
        struct
        {
            //
            // Distinct fields, NOT a union -- matching the real IRP, where
            // DriverContext[4] overlays KDEVICE_QUEUE_ENTRY and ListEntry
            // is a separate member further along Tail.Overlay. Modelling
            // them as a union looks harmless (an IRP is either queued or in
            // flight) but is wrong for exactly the case BlorgFS depends on:
            // an IRP re-queued by an async completion is parked on the CSQ
            // by ListEntry while still carrying IRP_CONTEXT_FLAG_NET_DONE
            // in DriverContext[0] and that completion's result in
            // DriverContext[1]. Overlaying them let the CSQ's list pointers
            // scribble over both, so the flags BlorgFspDispatch read back were
            // whatever RemoveEntryList had left there, and the NET_DONE
            // re-drive -- the whole point of the requeue -- could not be
            // tested at all.
            //
            PVOID DriverContext[4];
            LIST_ENTRY ListEntry;

            PVOID Thread;
            PIRP  CurrentStackLocation;
        } Overlay;
    } Tail;
};

PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
VOID IoFreeIrp(PIRP Irp);
VOID IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context, BOOLEAN Success, BOOLEAN Error, BOOLEAN Cancel);
VOID IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
BOOLEAN IoCancelIrp(PIRP Irp);
VOID IoMarkIrpPending(PIRP Irp);

typedef char CCHAR;

///////////////////////////////////////////////////////////////////////////
// Work items
///////////////////////////////////////////////////////////////////////////

//
// Queued work runs when the test drains it, at PASSIVE_LEVEL regardless
// of the IRQL that queued it -- which is the kernel's rule and the whole
// basis of the driver's bounce design. Draining explicitly is what lets a
// test observe the state between "a completion queued the pump" and "the
// pump ran", which is where the ring's ordering invariants live.
//
typedef struct _IO_WORKITEM IO_WORKITEM, * PIO_WORKITEM;

typedef VOID IO_WORKITEM_ROUTINE(PDEVICE_OBJECT DeviceObject, PVOID Context);
typedef IO_WORKITEM_ROUTINE* PIO_WORKITEM_ROUTINE;

typedef enum _WORK_QUEUE_TYPE
{
    CriticalWorkQueue = 0,
    DelayedWorkQueue = 1
} WORK_QUEUE_TYPE;

PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject);
VOID IoFreeWorkItem(PIO_WORKITEM IoWorkItem);
VOID IoQueueWorkItem(PIO_WORKITEM IoWorkItem, PIO_WORKITEM_ROUTINE Routine, WORK_QUEUE_TYPE QueueType, PVOID Context);

// Runs every queued work item, including any they queue in turn.
ULONG ShimDrainWorkItems(VOID);
ULONG ShimPendingWorkItems(VOID);

///////////////////////////////////////////////////////////////////////////
// Rtl string helpers
///////////////////////////////////////////////////////////////////////////

BOOLEAN RtlEqualString(const STRING* String1, const STRING* String2, BOOLEAN CaseInSensitive);
NTSTATUS RtlUnicodeStringToUTF8String(PUTF8_STRING Destination, const UNICODE_STRING* Source, BOOLEAN AllocateDestination);
VOID RtlFreeUTF8String(PUTF8_STRING Utf8String);
NTSTATUS RtlUTF8ToUnicodeN(PWSTR Destination, ULONG MaxBytes, PULONG ActualBytes, const CHAR* Source, ULONG SourceBytes);
NTSTATUS RtlStringCbLengthA(const char* Source, SIZE_T MaxLength, SIZE_T* Length);
NTSTATUS RtlStringCbPrintfA(char* Destination, SIZE_T DestinationBytes, const char* Format, ...);

///////////////////////////////////////////////////////////////////////////
// Tracing
///////////////////////////////////////////////////////////////////////////

extern BOOLEAN ShimTraceEnabled;

//
// BLORGFS_PRINT and BLORGFS_LOG belong to Driver.h and are not redefined
// here -- redefining them would mean the sandbox exercises different
// tracing than the driver ships, and would silence the DBG build's
// formatting entirely. What the sandbox substitutes is the kernel API
// underneath, DbgPrintEx, which is the actual thing usermode lacks.
//
#define DPFLTR_DEFAULT_ID 0
#define DPFLTR_INFO_LEVEL 3
#define DPFLTR_ERROR_LEVEL 0

ULONG DbgPrintEx(ULONG ComponentId, ULONG Level, const char* Format, ...);

//
// Statistics: Driver.h includes the real Statistics.h, whose kernel
// branch declares the counter block, the accessors and the
// BLORGFS_STAT_* macros. NtShim.c implements the accessors. Nothing is
// re-declared here.
//
LONG64 BlorgStatisticsNow(VOID);

///////////////////////////////////////////////////////////////////////////
// Model lifecycle for a test case
///////////////////////////////////////////////////////////////////////////

VOID ShimReset(VOID);

#ifdef __cplusplus
}
#endif

///////////////////////////////////////////////////////////////////////////
// Interlocked operations under systematic exploration
///////////////////////////////////////////////////////////////////////////

//
// Redirected so every atomic is a scheduling point. Outside an
// exploration each of these is the intrinsic plus a call that returns
// immediately, so ordinary tests pay a call and nothing else.
//
// Placed last on purpose: everything above uses the real Win32 macros
// while this header is being read, and only code compiled AFTER including
// Driver.h -- which is every driver translation unit -- sees the
// redirection.
//
#include "Scheduler.h"

//
// Shim translation units opt out by defining BLORGFS_SHIM_INTERNAL before
// including Driver.h.
//
// They must. The shim's own one-time initialisers -- EnsureWorkItemCs, the
// TLS slot setup, the lock-order critical section -- are guarded by an
// InterlockedCompareExchange, and turning that into a scheduling point
// makes the FIRST replay take the init path and every later one take the
// fast path. Different yield counts for the same schedule prefix, which
// the explorer correctly reports as replay divergence and which took three
// wrong guesses to find. Scheduling points belong in driver code, not in
// the scaffolding underneath it.
//
#ifndef BLORGFS_SHIM_INTERNAL

#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedExchange
#undef InterlockedCompareExchange
#undef InterlockedIncrement64
#undef InterlockedDecrement64

#define InterlockedIncrement            KmSchedInterlockedIncrement
#define InterlockedDecrement            KmSchedInterlockedDecrement
#define InterlockedExchange             KmSchedInterlockedExchange
#define InterlockedCompareExchange      KmSchedInterlockedCompareExchange
#define InterlockedIncrement64          KmSchedInterlockedIncrement64
#define InterlockedDecrement64          KmSchedInterlockedDecrement64

#endif
