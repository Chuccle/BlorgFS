#include "Driver.h"

static NTSTATUS BlorgOpenExistingFcbShared(PIRP Irp, PFILE_OBJECT FileObject, PACCESS_MASK DesiredAccess, USHORT ShareAccess, PFCB Fcb)
{
    if (1 == InterlockedIncrement64(&Fcb->RefCount))
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess);
    }
    else
    {
        NTSTATUS result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&Fcb->RefCount);
            return result;
        }
    }

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = NULL;
    FileObject->SectionObjectPointer = &Fcb->NonPaged->SectionObjectPointers;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgOpenExistingDcbShared(PIRP Irp, PFILE_OBJECT FileObject, PACCESS_MASK DesiredAccess, USHORT ShareAccess, PDCB Dcb, PDEVICE_OBJECT VolumeDeviceObject)
{
    PCCB pCcb;

    NTSTATUS result = BlorgCreateCCB(&pCcb, VolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    if (1 == InterlockedIncrement64(&Dcb->RefCount))
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &Dcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &Dcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&Dcb->RefCount);
            BlorgFreeFileContext(pCcb, VolumeDeviceObject);
            return result;
        }
    }

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Dcb;
    FileObject->FsContext2 = pCcb;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgOpenExistingFcbExclusive(PIRP Irp, PFILE_OBJECT FileObject, PACCESS_MASK DesiredAccess, USHORT ShareAccess, PFCB Fcb)
{
    if (0 == Fcb->RefCount)
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess);
    }
    else
    {
        NTSTATUS result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            return result;
        }
    }

    Fcb->RefCount++;

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = NULL;
    FileObject->SectionObjectPointer = &Fcb->NonPaged->SectionObjectPointers;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgOpenExistingDcbExclusive(PIRP Irp, PFILE_OBJECT FileObject, PACCESS_MASK DesiredAccess, USHORT ShareAccess, PDCB Dcb, PDEVICE_OBJECT VolumeDeviceObject)
{
    PCCB pCcb;

    NTSTATUS result = BlorgCreateCCB(&pCcb, VolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    if (0 == Dcb->RefCount)
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, &Dcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, &Dcb->ShareAccess, TRUE);

        if (!NT_SUCCESS(result))
        {
            BlorgFreeFileContext(pCcb, VolumeDeviceObject);
            return result;
        }
    }

    Dcb->RefCount++;

#pragma warning(suppress: 28175) // We are a filesystem. We are allowed to fiddle with VPB.
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Dcb;
    FileObject->FsContext2 = pCcb;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

static NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    // handle the volume object's create request
    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PFILE_OBJECT relatedFileObject = fileObject->RelatedFileObject;

    UNICODE_STRING filePath = fileObject->FileName;
    ULONG options = IrpSp->Parameters.Create.Options;
    USHORT shareAccess = IrpSp->Parameters.Create.ShareAccess;
    UCHAR createDisposition = (options >> 24) & 0x000000ff;
    PACCESS_MASK desiredAccess = &IrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    if ((FILE_OPEN != createDisposition) &&
        (FILE_OPEN_IF != createDisposition))
    {
        return STATUS_ACCESS_DENIED;
    }

    if (0 == *desiredAccess)
    {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Only allow to be opened for read or execute access.
    //

    if (FlagOn(*desiredAccess, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) != *desiredAccess)
    {
        return STATUS_ACCESS_DENIED;
    }

    if (!relatedFileObject)
    {
        // open the volume object
        if (0 == filePath.Length)
        {
            return BlorgOpenExistingFcbShared(Irp, fileObject, desiredAccess, shareAccess, GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb);
        }

        // open the root directory
        if (sizeof(WCHAR) == filePath.Length && L'\\' == filePath.Buffer[0])
        {
            if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
            {
                return STATUS_INVALID_PARAMETER;
            }

            return BlorgOpenExistingDcbShared(Irp, fileObject, desiredAccess, shareAccess, GetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb, VolumeDeviceObject);
        }
    }
    else
    {
        KdBreakPoint();
    }

    if (((sizeof(WCHAR) * 2) <= filePath.Length) &&
        (L'\\' == filePath.Buffer[(filePath.Length / sizeof(WCHAR)) - 1]))
    {
        filePath.Length -= sizeof(WCHAR);
    }

    BLORGFS_PRINT(" ->NormalisedFileName             = %wZ\n", &filePath);

    PVCB vcb = GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb;

    // lookup the in memory FCBs to see if we already have this file open
    ExAcquireResourceSharedLite(vcb->Header.Resource, TRUE);

    PCOMMON_CONTEXT desiredNode = SearchByPath(GetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb, &filePath);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);
                    return STATUS_FILE_IS_A_DIRECTORY;
                }

                NTSTATUS result = BlorgOpenExistingDcbShared(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                NTSTATUS result = BlorgOpenExistingFcbShared(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    // Verify that this actually exists on the remote store
    BOOLEAN isDir;
    DIRECTORY_ENTRY dirEntInfo;

    NTSTATUS result = GetHttpFileInformation(&filePath, &dirEntInfo, &isDir);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    if (isDir && BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
    {
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    // slow case, we're missing DCBs in memory which represent each component of the path
    // we need to create them and insert them into the tree
    ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);

    // Recheck at this point as A lot of uOps have transpired since we were last sync'd 
    desiredNode = SearchByPath(GetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb, &filePath);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);
                    return STATUS_FILE_IS_A_DIRECTORY;
                }

                result = BlorgOpenExistingDcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                result = BlorgOpenExistingFcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
        }
    }

    // insert by path should ensure the fcb is not already in the tree
    result = InsertByPath(GetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb, &filePath, &dirEntInfo, isDir, VolumeDeviceObject, &desiredNode);

    if (!NT_SUCCESS(result))
    {
        ExReleaseResourceLite(vcb->Header.Resource);
        return result;
    }

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                result = BlorgOpenExistingDcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                result = BlorgOpenExistingFcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                ExReleaseResourceLite(vcb->Header.Resource);
                return result;
            }
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    KdBreakPoint();
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

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeCreate(Irp, irpSp, DeviceObject);
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
    FsRtlExitFileSystem();

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
