//
// The FsRtl surface Structs.c calls out to, plus the volume scaffolding a
// node table needs to exist at all.
//
// Everything here is deliberately inert. The node table's lifetime
// protocol does not read inside an oplock or a byte-range lock -- it
// manipulates counts, list linkage and push locks -- so modelling those
// with real behaviour would add surface without adding a single assertion
// the tests could make. What is real is anything the protocol touches,
// and that all lives in NtShim/NtShimSync.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

#include <stdlib.h>
#include <string.h>

VOID FsRtlSetupAdvancedHeader(PVOID Header, PFAST_MUTEX FastMutex)
{
    PFSRTL_ADVANCED_FCB_HEADER header = (PFSRTL_ADVANCED_FCB_HEADER)Header;

    header->FastMutex = FastMutex;
    InitializeListHead(&header->FilterContexts);
}

VOID FsRtlTeardownPerStreamContexts(PFSRTL_ADVANCED_FCB_HEADER Header)
{
    (void)Header;
}

VOID FsRtlInitializeFileLock(PFILE_LOCK FileLock, PVOID CompleteLockRoutine, PVOID UnlockRoutine)
{
    (void)CompleteLockRoutine;
    (void)UnlockRoutine;

    memset(FileLock, 0, sizeof(*FileLock));
}

VOID FsRtlUninitializeFileLock(PFILE_LOCK FileLock)
{
    (void)FileLock;
}

VOID FsRtlInitializeOplock(POPLOCK Oplock)
{
    memset(Oplock, 0, sizeof(*Oplock));
}

VOID FsRtlUninitializeOplock(POPLOCK Oplock)
{
    (void)Oplock;
}

//
// Splits "a\b\c" into "a" and "b\c", the way the real one does. This is
// not inert: BlorgInsertByPath walks a path component by component to build
// the DCB chain, so getting the split wrong would build a different tree
// than the driver does and every structural assertion would be measuring
// the wrong thing.
//
NTSTATUS FsRtlDissectName(UNICODE_STRING Path, PUNICODE_STRING FirstName, PUNICODE_STRING RemainingName)
{
    FirstName->Length = 0;
    FirstName->MaximumLength = 0;
    FirstName->Buffer = Path.Buffer;

    RemainingName->Length = 0;
    RemainingName->MaximumLength = 0;
    RemainingName->Buffer = NULL;

    USHORT chars = Path.Length / sizeof(WCHAR);
    USHORT start = 0;

    while (start < chars && Path.Buffer[start] == L'\\')
    {
        start++;
    }

    USHORT end = start;

    while (end < chars && Path.Buffer[end] != L'\\')
    {
        end++;
    }

    FirstName->Buffer = Path.Buffer + start;
    FirstName->Length = (USHORT)((end - start) * sizeof(WCHAR));
    FirstName->MaximumLength = FirstName->Length;

    USHORT remaining = end;

    while (remaining < chars && Path.Buffer[remaining] == L'\\')
    {
        remaining++;
    }

    if (remaining < chars)
    {
        RemainingName->Buffer = Path.Buffer + remaining;
        RemainingName->Length = (USHORT)((chars - remaining) * sizeof(WCHAR));
        RemainingName->MaximumLength = RemainingName->Length;
    }

    return STATUS_SUCCESS;
}

//
// Driver.h declares the driver-wide global as extern; Driver.c defines it,
// and Driver.c is not one of the translation units under test. One
// definition here serves every sandbox target, and it is the driver's own
// struct rather than a stand-in, so a field added to it appears here too.
//
struct GLOBAL global;

///////////////////////////////////////////////////////////////////////////
// Volume scaffolding
///////////////////////////////////////////////////////////////////////////

//
// A device object is opaque to the driver except through
// BlorgGetVolumeDeviceExtension, so the model allocates one whose extension is
// a real BLORGFS_VDO_DEVICE_EXTENSION with real lookaside lists -- the
// nodes under test come out of those, and their accounting is what proves
// a reap actually freed something.
//
//
// A device object and the extension it points at. BlorgGetVolumeDeviceExtension
// reads DeviceObject->DeviceExtension, so the extension has to hang off
// the object rather than be overlaid on it -- overlaying reads the
// extension's first eight bytes as the pointer and faults on first use.
//
typedef struct _MODEL_DEVICE_OBJECT
{
    DEVICE_OBJECT Object;
    BLORGFS_VDO_DEVICE_EXTENSION Extension;
} MODEL_DEVICE_OBJECT;

static MODEL_DEVICE_OBJECT* ModelDevice = NULL;

PDEVICE_OBJECT StructsModelCreateVolume(VOID)
{
    ModelDevice = (MODEL_DEVICE_OBJECT*)calloc(1, sizeof(MODEL_DEVICE_OBJECT));

    ModelDevice->Object.DeviceExtension = &ModelDevice->Extension;

    BLORGFS_VDO_DEVICE_EXTENSION* extension = &ModelDevice->Extension;

    ExInitializeNPagedLookasideList(&extension->NonPagedNodeLookasideList, NULL, NULL, 0, sizeof(NON_PAGED_NODE), 'NPN', 0);
    ExInitializePagedLookasideList(&extension->FcbLookasideList, NULL, NULL, 0, sizeof(FCB), 'FCB', 0);
    ExInitializePagedLookasideList(&extension->DcbLookasideList, NULL, NULL, 0, sizeof(DCB), 'DCB', 0);
    ExInitializePagedLookasideList(&extension->CcbLookasideList, NULL, NULL, 0, sizeof(CCB), 'CCB', 0);

    InitializeListHead(&extension->NotifyList);

    return (PDEVICE_OBJECT)ModelDevice;
}

VOID StructsModelDestroyVolume(PDEVICE_OBJECT VolumeDeviceObject)
{
    MODEL_DEVICE_OBJECT* device = (MODEL_DEVICE_OBJECT*)VolumeDeviceObject;

    if (!device)
    {
        return;
    }

    ExDeleteNPagedLookasideList(&device->Extension.NonPagedNodeLookasideList);
    ExDeletePagedLookasideList(&device->Extension.FcbLookasideList);
    ExDeletePagedLookasideList(&device->Extension.DcbLookasideList);
    ExDeletePagedLookasideList(&device->Extension.CcbLookasideList);

    free(device);
    ModelDevice = NULL;
}

BOOLEAN FsRtlCheckLockForOplockRequest(PFILE_LOCK FileLock, PLARGE_INTEGER AllocationSize)
{
    (void)FileLock;
    (void)AllocationSize;
    return TRUE;
}

NTSTATUS FsRtlOplockFsctrl(POPLOCK Oplock, PIRP Irp, ULONG OpenCount)
{
    (void)Oplock;
    (void)Irp;
    (void)OpenCount;
    return STATUS_SUCCESS;
}
