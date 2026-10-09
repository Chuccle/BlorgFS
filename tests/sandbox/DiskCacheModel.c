//
// The file system under the disk cache's file: see DiskCacheModel.h.
//
// One volume, one file on it and the directory it is in. The file's handle
// is the address of its FILE_OBJECT, which is what ObReferenceObjectByHandle
// hands back for a file handle (DispatchModel.c). Its bytes live in CRT
// memory rather than pool: they belong to another file system, and the
// driver's pool accounting must not see them.
//
// IoCallDriver checks what any file system would refuse of a handle
// opened FILE_NO_INTERMEDIATE_BUFFERING -- an offset or length that is not
// sector aligned, or an MDL that does not describe Length bytes -- and
// that an IRP sent down carries the non-cached flag and a completion
// routine that takes it back. It reports a violation rather than failing
// the IRP, since a real file system would fail it in a way the cache reads
// as its file being lost, and the test would pass for the wrong reason.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

#define DISK_CACHE_MODEL_SECTOR 512
#define DISK_CACHE_MODEL_HELD_MAX 1024

static struct
{
    DEVICE_OBJECT Device;
    FILE_OBJECT File;
    FILE_OBJECT Directory;
    PUCHAR Store;
    SIZE_T StoreSize;
    PSID Owner;
    UCHAR FailMajor;
    NTSTATUS FailStatus;
    BOOLEAN Hold;
    ULONG HeldCount;
    PIRP Held[DISK_CACHE_MODEL_HELD_MAX];
    ULONG Reads;
    ULONG Writes;
} Model;

VOID DiskCacheModelReset(VOID)
{
    free(Model.Store);
    memset(&Model, 0, sizeof(Model));

    Model.Device.StackSize = 1;
    Model.Device.SectorSize = DISK_CACHE_MODEL_SECTOR;
    Model.File.DeviceObject = &Model.Device;
    Model.Directory.DeviceObject = &Model.Device;
    Model.Owner = SeExports->SeAliasAdminsSid;
}

PDEVICE_OBJECT DiskCacheModelDevice(VOID)
{
    return &Model.Device;
}

VOID DiskCacheModelSetOwner(PSID Owner)
{
    Model.Owner = Owner;
}

VOID DiskCacheModelFailNext(UCHAR MajorFunction, NTSTATUS Status)
{
    Model.FailMajor = MajorFunction;
    Model.FailStatus = Status;
}

VOID DiskCacheModelHold(BOOLEAN Hold)
{
    Model.Hold = Hold;
}

ULONG DiskCacheModelIrps(UCHAR MajorFunction)
{
    return (IRP_MJ_READ == MajorFunction) ? Model.Reads : Model.Writes;
}

NTSTATUS ZwCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
    (void)DesiredAccess; (void)AllocationSize; (void)FileAttributes;
    (void)CreateDisposition; (void)EaBuffer; (void)EaLength;

    KmRequireIrqlAtMost(PASSIVE_LEVEL, "ZwCreateFile");

    if (!BooleanFlagOn(ObjectAttributes->Attributes, OBJ_DONT_REPARSE) ||
        !BooleanFlagOn(CreateOptions, FILE_OPEN_REPARSE_POINT))
    {
        KmReportViolation(KmViolationLifetime,
            "the disk cache opened a path that may follow a reparse point a user planted");
    }

    if (BooleanFlagOn(ShareAccess, FILE_SHARE_DELETE | FILE_SHARE_WRITE))
    {
        KmReportViolation(KmViolationLifetime,
            "the disk cache's file shared so that something else could change or delete it");
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = 0;

    *FileHandle = BooleanFlagOn(CreateOptions, FILE_DIRECTORY_FILE) ? (HANDLE)&Model.Directory : (HANDLE)&Model.File;

    return STATUS_SUCCESS;
}

NTSTATUS ZwSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "ZwSetInformationFile");

    if ((HANDLE)&Model.File != FileHandle || FileEndOfFileInformation != FileInformationClass ||
        sizeof(FILE_END_OF_FILE_INFORMATION) != Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const SIZE_T size = (SIZE_T)((PFILE_END_OF_FILE_INFORMATION)FileInformation)->EndOfFile.QuadPart;
    PUCHAR store = (PUCHAR)calloc(1, size);

    if (!store)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    free(Model.Store);
    Model.Store = store;
    Model.StoreSize = size;

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = 0;

    return STATUS_SUCCESS;
}

NTSTATUS ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    (void)Handle; (void)Alertable; (void)Timeout;

    return STATUS_SUCCESS;
}

//
// A self-relative descriptor naming Model.Owner, built by the Win32 calls
// RtlGetOwnerSecurityDescriptor then reads it back with.
//
NTSTATUS ZwQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded)
{
    (void)Handle;

    SECURITY_DESCRIPTOR absolute;
    DWORD length = Length;

    if (OWNER_SECURITY_INFORMATION != SecurityInformation ||
        !InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&absolute, Model.Owner, FALSE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!MakeSelfRelativeSD(&absolute, SecurityDescriptor, &length))
    {
        *LengthNeeded = length;
        return STATUS_BUFFER_TOO_SMALL;
    }

    *LengthNeeded = length;

    return STATUS_SUCCESS;
}

NTSTATUS RtlGetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, PSID* Owner, PBOOLEAN OwnerDefaulted)
{
    BOOL defaulted = FALSE;

    if (!GetSecurityDescriptorOwner(SecurityDescriptor, Owner, &defaulted))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *OwnerDefaulted = defaulted ? TRUE : FALSE;

    return STATUS_SUCCESS;
}

BOOLEAN RtlEqualSid(PSID Sid1, PSID Sid2)
{
    return EqualSid(Sid1, Sid2) ? TRUE : FALSE;
}

NTSTATUS RtlCreateAcl(PACL Acl, ULONG AclLength, ULONG AclRevision)
{
    return InitializeAcl(Acl, AclLength, AclRevision) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

NTSTATUS RtlAddAccessAllowedAce(PACL Acl, ULONG AceRevision, ACCESS_MASK AccessMask, PSID Sid)
{
    return AddAccessAllowedAce(Acl, AceRevision, AccessMask, Sid) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT FileObject)
{
    return FileObject->DeviceObject;
}

PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP Irp)
{
    return Irp->StackLocation;
}

//
// Does what Irp asks of the file and hands it back to its completion
// routine at DISPATCH_LEVEL, which must keep it.
//
static VOID DiskCacheModelComplete(PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = Irp->StackLocation;
    const BOOLEAN read = (IRP_MJ_READ == irpSp->MajorFunction);
    const ULONG length = read ? irpSp->Parameters.Read.Length : irpSp->Parameters.Write.Length;
    const ULONG64 offset = (ULONG64)(read ? irpSp->Parameters.Read.ByteOffset.QuadPart : irpSp->Parameters.Write.ByteOffset.QuadPart);

    if (Model.FailMajor == irpSp->MajorFunction)
    {
        Model.FailMajor = 0;
        Irp->IoStatus.Status = Model.FailStatus;
        Irp->IoStatus.Information = 0;
    }
    else if (offset + length > Model.StoreSize)
    {
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
    }
    else
    {
        if (read)
        {
            memcpy(Irp->MdlAddress->Base, Model.Store + offset, length);
        }
        else
        {
            memcpy(Model.Store + offset, Irp->MdlAddress->Base, length);
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = length;
    }

    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);

    if (STATUS_MORE_PROCESSING_REQUIRED != Irp->CompletionRoutine(&Model.Device, Irp, Irp->CompletionContext))
    {
        KmReportViolation(KmViolationLifetime,
            "the completion routine of an IRP the driver allocated let the I/O manager complete it");
    }

    KeLowerIrql(oldIrql);
}

NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KmRequireIrqlAtMost(APC_LEVEL, "IoCallDriver to a file system");

    PIO_STACK_LOCATION irpSp = Irp->StackLocation;
    const BOOLEAN read = (IRP_MJ_READ == irpSp->MajorFunction);
    const ULONG length = read ? irpSp->Parameters.Read.Length : irpSp->Parameters.Write.Length;
    const ULONG64 offset = (ULONG64)(read ? irpSp->Parameters.Read.ByteOffset.QuadPart : irpSp->Parameters.Write.ByteOffset.QuadPart);

    if (&Model.Device != DeviceObject || &Model.File != irpSp->FileObject || &Model.File != Irp->Tail.Overlay.OriginalFileObject ||
        (!read && IRP_MJ_WRITE != irpSp->MajorFunction))
    {
        KmReportViolation(KmViolationLifetime, "an IRP sent somewhere other than to the disk cache's file");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_NOCACHE) ||
        !BooleanFlagOn(Irp->Flags, read ? IRP_READ_OPERATION : IRP_WRITE_OPERATION) ||
        !Irp->CompletionRoutine || !Irp->MdlAddress || Irp->MdlAddress->Length != length ||
        0 != ((offset | length) & (DISK_CACHE_MODEL_SECTOR - 1)))
    {
        KmReportViolation(KmViolationLifetime,
            "a non-buffered cache file IRP a file system would refuse: not sector aligned, "
            "flags or MDL not matching what it asks");
    }

    if (read)
    {
        Model.Reads++;
    }
    else
    {
        Model.Writes++;
    }

    if (Model.Hold && Model.HeldCount < DISK_CACHE_MODEL_HELD_MAX)
    {
        Model.Held[Model.HeldCount++] = Irp;
        return STATUS_PENDING;
    }

    DiskCacheModelComplete(Irp);

    return STATUS_PENDING;
}

ULONG DiskCacheModelCompleteHeld(VOID)
{
    ULONG completed = 0;

    while (completed < Model.HeldCount)
    {
        DiskCacheModelComplete(Model.Held[completed++]);
    }

    Model.HeldCount = 0;

    return completed;
}
