#include "Driver.h"

static NTSTATUS BlorgVolumeOpen(PIRP Irp, PFILE_OBJECT FileObject, UCHAR CreateDisposition, USHORT ShareAccess, PACCESS_MASK DesiredAccess)
{
    if (0 == *DesiredAccess)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (0 == ShareAccess)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((CreateDisposition != FILE_OPEN) &&
        (CreateDisposition != FILE_OPEN_IF))
    {
        return STATUS_ACCESS_DENIED;
    }

    //
    // Only allow the volume to be opened for read access.
    //

    if (FlagOn(*DesiredAccess, ~FILE_GENERIC_READ))
    {
        return STATUS_ACCESS_DENIED;
    }

    PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;

    PVCB pVcb = GetVolumeDeviceExtension(pVolumeDeviceObject)->Vcb;

    if (InterlockedIncrement64(&pVcb->RefCount) == 1)
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &pVcb->ShareAccess);
    }
    else
    {
        NTSTATUS result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &pVcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&pVcb->RefCount);
            return result;
        }
    }

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = pVcb;
    FileObject->FsContext2 = NULL;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgRootDirectoryOpen(PIRP Irp, PFILE_OBJECT FileObject, UCHAR CreateDisposition, PACCESS_MASK DesiredAccess, USHORT ShareAccess)
{
    if (0 == *DesiredAccess)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (0 == ShareAccess)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((FILE_OPEN != CreateDisposition) &&
        (FILE_OPEN_IF != CreateDisposition))
    {
        return STATUS_ACCESS_DENIED;
    }

    //
    // Only allow the root directory to be opened for read access.
    //

    if (FlagOn(*DesiredAccess, ~FILE_GENERIC_READ))
    {
        return STATUS_ACCESS_DENIED;
    }

    PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;

    PDCB pRootDcb = GetVolumeDeviceExtension(pVolumeDeviceObject)->RootDcb;

    PCCB pCcb;

    NTSTATUS result = BlorgCreateCCB(&pCcb, pVolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    if (InterlockedIncrement64(&pRootDcb->RefCount) == 1)
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &pRootDcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &pRootDcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&pRootDcb->RefCount);
            BlorgFreeFileContext(pCcb);
            return result;
        }
    }

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = pRootDcb;
    FileObject->FsContext2 = pCcb;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    // handle the volume object's create request
    PFILE_OBJECT pFileObject = IrpSp->FileObject;
    PFILE_OBJECT pRelatedFileObject = pFileObject->RelatedFileObject;
    UNICODE_STRING fileName = pFileObject->FileName;

    ULONG options = IrpSp->Parameters.Create.Options;
    USHORT shareAccess = IrpSp->Parameters.Create.ShareAccess;
    UCHAR createDisposition = (options >> 24) & 0x000000ff;
    PACCESS_MASK pDesiredAccess = &IrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    if (!pRelatedFileObject)
    {
        // open the volume object
        if (0 == fileName.Length)
        {
            return BlorgVolumeOpen(Irp, pFileObject, createDisposition, shareAccess, pDesiredAccess);
        }

        // open the root directory
        if (sizeof(WCHAR) == fileName.Length && L'\\' == fileName.Buffer[0])
        {
            if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
            {
                return STATUS_FILE_IS_A_DIRECTORY;

            }

            return BlorgRootDirectoryOpen(Irp, pFileObject, createDisposition, pDesiredAccess, shareAccess);
        }
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS BlorgDiskCreate(PIRP Irp)
{
    // handle the disk object's create request
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static NTSTATUS BlorgFileSystemCreate(PIRP Irp)
{
    // handle the FileSystem object's create request
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeCreate(Irp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            result = BlorgDiskCreate(Irp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            result = BlorgFileSystemCreate(Irp);
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
