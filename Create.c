#include "Driver.h"

static inline BOOLEAN CheckFileAccess(const PACCESS_MASK DesiredAccess, BOOLEAN IsReadOnly)
{
    //
    // Reject only truly unsupported flags (same as FAT)
    //

    if (FlagOn(*DesiredAccess, ~(DELETE |
        READ_CONTROL |
        WRITE_OWNER |
        WRITE_DAC |
        SYNCHRONIZE |
        ACCESS_SYSTEM_SECURITY |
        FILE_WRITE_DATA |
        FILE_READ_EA |
        FILE_WRITE_EA |
        FILE_READ_ATTRIBUTES |
        FILE_WRITE_ATTRIBUTES |
        FILE_LIST_DIRECTORY |
        FILE_TRAVERSE |
        FILE_DELETE_CHILD |
        FILE_APPEND_DATA |
        MAXIMUM_ALLOWED)))
    {
        KdBreakPoint();
        return FALSE;
    }

    if (IsReadOnly)
    {
        ACCESS_MASK AccessMask = DELETE | READ_CONTROL | WRITE_OWNER | WRITE_DAC |
            SYNCHRONIZE | ACCESS_SYSTEM_SECURITY | FILE_READ_DATA |
            FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES |
            FILE_WRITE_ATTRIBUTES | FILE_EXECUTE | FILE_LIST_DIRECTORY |
            FILE_TRAVERSE;

        if (FlagOn(*DesiredAccess, ~AccessMask))
        {
            KdBreakPoint();
            return FALSE;
        }
    }

    return TRUE;
}

static inline BOOLEAN CheckDirectoryAccess(const PACCESS_MASK DesiredAccess, BOOLEAN IsReadOnly)
{
    //
    // Reject only truly unsupported flags (same as FAT)
    //
    
    if (FlagOn(*DesiredAccess, ~(DELETE |
        READ_CONTROL |
        WRITE_OWNER |
        WRITE_DAC |
        SYNCHRONIZE |
        ACCESS_SYSTEM_SECURITY |
        FILE_WRITE_DATA |
        FILE_READ_EA |
        FILE_WRITE_EA |
        FILE_READ_ATTRIBUTES |
        FILE_WRITE_ATTRIBUTES |
        FILE_LIST_DIRECTORY |
        FILE_TRAVERSE |
        FILE_DELETE_CHILD |
        FILE_APPEND_DATA |
        MAXIMUM_ALLOWED)))
    {
        KdBreakPoint();
        return FALSE;
    }

    if (IsReadOnly)
    {
        ACCESS_MASK AccessMask = DELETE | READ_CONTROL | WRITE_OWNER | WRITE_DAC |
            SYNCHRONIZE | ACCESS_SYSTEM_SECURITY | FILE_READ_DATA |
            FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES |
            FILE_WRITE_ATTRIBUTES | FILE_EXECUTE | FILE_LIST_DIRECTORY |
            FILE_TRAVERSE;

         //
         //  If this is a subdirectory also allow add file/directory and delete.
         //
         
        AccessMask |= FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD;

        if (FlagOn(*DesiredAccess, ~AccessMask))
        {
            KdBreakPoint();
            return FALSE;
        }
    }

    return TRUE;
}

static inline NTSTATUS OpenExistingFcbShared(PIRP Irp, PFILE_OBJECT FileObject, const PACCESS_MASK DesiredAccess, USHORT ShareAccess, PFCB Fcb)
{

    if (!CheckFileAccess(DesiredAccess, TRUE))
    {
        return STATUS_ACCESS_DENIED;
    }

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

static inline NTSTATUS OpenExistingDcbShared(PIRP Irp, PFILE_OBJECT FileObject, const PACCESS_MASK DesiredAccess, USHORT ShareAccess, PDCB Dcb, const PDEVICE_OBJECT VolumeDeviceObject)
{
    if (!CheckDirectoryAccess(DesiredAccess, TRUE))
    {
        return STATUS_ACCESS_DENIED;
    }

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

static inline NTSTATUS BlorgOpenExistingFcbExclusive(PIRP Irp, PFILE_OBJECT FileObject, const PACCESS_MASK DesiredAccess, USHORT ShareAccess, PFCB Fcb)
{
    if (!CheckFileAccess(DesiredAccess, TRUE))
    {
        return STATUS_ACCESS_DENIED;
    }

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

static inline NTSTATUS OpenExistingDcbExclusive(PIRP Irp, PFILE_OBJECT FileObject, const PACCESS_MASK DesiredAccess, USHORT ShareAccess, PDCB Dcb, PDEVICE_OBJECT VolumeDeviceObject)
{
    if (!CheckDirectoryAccess(DesiredAccess, TRUE))
    {
        return STATUS_ACCESS_DENIED;
    }

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

NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    struct OwnedString
    {
        BOOLEAN IsAllocated;
        UNICODE_STRING String;
    };

    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PFILE_OBJECT relatedFileObject = fileObject->RelatedFileObject;

    struct OwnedString filePath = { 0 };
    PDCB parentDcb = GetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb;
    ULONG options = IrpSp->Parameters.Create.Options;
    USHORT shareAccess = IrpSp->Parameters.Create.ShareAccess;
    UCHAR createDisposition = (options >> 24) & 0x000000ff;
    PACCESS_MASK desiredAccess = &IrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    if (FILE_OPEN != createDisposition && FILE_OPEN_IF != createDisposition)
    {
        KdBreakPoint();
        return STATUS_ACCESS_DENIED;
    }

    if (!relatedFileObject)
    {
        //
        // Open the volume object
        //

        if (0 == fileObject->FileName.Length)
        {
            return OpenExistingFcbShared(Irp, fileObject, desiredAccess, shareAccess, GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb);
        }
        
        filePath.String = fileObject->FileName;
    }
    else
    {
        //
        //  A relative open must be via a relative path.
        //

        if ((0 < fileObject->FileName.Length) &&
            (L'\\' == fileObject->FileName.Buffer[0]))
        {
            return STATUS_OBJECT_NAME_INVALID;
        }
        
        //
        // Validate the related file object is a DCB
        //
        
        if ((BLORGFS_DCB_SIGNATURE != GET_NODE_TYPE(relatedFileObject->FsContext)) 
            && (BLORGFS_ROOT_DCB_SIGNATURE != GET_NODE_TYPE(relatedFileObject->FsContext)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        parentDcb = relatedFileObject->FsContext;

        //
        //  Common path + path separator + remaining path
        //
        
        USHORT length = relatedFileObject->FileName.Length + sizeof(WCHAR) + fileObject->FileName.Length;

        if (0 < length)
        { 
            filePath.String.Buffer = ExAllocatePoolUninitialized(PagedPool, length, 'CRET');

            if (!filePath.String.Buffer)
            {
                return STATUS_NO_MEMORY;
            }

            filePath.IsAllocated = TRUE;

            RtlCopyMemory(filePath.String.Buffer, relatedFileObject->FileName.Buffer, relatedFileObject->FileName.Length);
            filePath.String.Buffer[relatedFileObject->FileName.Length / sizeof(WCHAR)] = L'\\';
            RtlCopyMemory(filePath.String.Buffer + relatedFileObject->FileName.Length, fileObject->FileName.Buffer, fileObject->FileName.Length);

            filePath.String.Length = length;
            filePath.String.MaximumLength = length;
        }
    }

    if (((sizeof(WCHAR) * 2) <= filePath.String.Length) &&
        (L'\\' == filePath.String.Buffer[(filePath.String.Length / sizeof(WCHAR)) - 1]))
    {
        filePath.String.Length -= sizeof(WCHAR);
    }
    
    //
    // Open the root directory
    //
    
    if (sizeof(WCHAR) == filePath.String.Length && L'\\' == filePath.String.Buffer[0])
    {
        if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
        {
            if(filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }
            
            return STATUS_FILE_IS_A_DIRECTORY;
        }

        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }
        
        return OpenExistingDcbShared(Irp, fileObject, desiredAccess, shareAccess, parentDcb, VolumeDeviceObject);
    }

    BLORGFS_PRINT(" ->NormalisedFileName             = %wZ\n", &filePath.String);

    PVCB vcb = GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb;

    // lookup the in memory FCBs to see if we already have this file open
    ExAcquireResourceSharedLite(vcb->Header.Resource, TRUE);

    PCOMMON_CONTEXT desiredNode = SearchByPath(parentDcb, &filePath.String);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_FILE_IS_A_DIRECTORY;
                }

                NTSTATUS result = OpenExistingDcbShared(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                
                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_NOT_A_DIRECTORY;
                }

                NTSTATUS result = OpenExistingFcbShared(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                
                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    // Verify that this actually exists on the remote store
    if (!BooleanFlagOn((ULONG_PTR)Irp->Tail.Overlay.DriverContext[0], IRP_CONTEXT_FLAG_IN_FSP))
    {
        BLORGFS_PRINT("BlorgVolumeCreate: Enqueue to Fsp\n");
        return FsdPostRequest(Irp, IrpSp);
    }
    
    DIRECTORY_ENTRY_METADATA dirEntInfo;
    
    NTSTATUS result = GetHttpFileInformation(&filePath.String, &dirEntInfo);

    if (!NT_SUCCESS(result))
    {
        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }
        
        return result;
    }

    if (dirEntInfo.IsDirectory && BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
    {
        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }

        return STATUS_FILE_IS_A_DIRECTORY;
    } 
    else if (!dirEntInfo.IsDirectory && BooleanFlagOn(options, FILE_DIRECTORY_FILE))
    { 
        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }
    
        return STATUS_NOT_A_DIRECTORY;
    }

    // slow case, we're missing DCBs in memory which represent each component of the path
    // we need to create them and insert them into the tree
    ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);

    // Recheck at this point as A lot of uOps have transpired since we were last sync'd 
    desiredNode = SearchByPath(parentDcb, &filePath.String);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);
                    
                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }
                    
                    return STATUS_FILE_IS_A_DIRECTORY;
                }

                result = OpenExistingDcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                
                ExReleaseResourceLite(vcb->Header.Resource);
                
                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_NOT_A_DIRECTORY;
                }

                result = BlorgOpenExistingFcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                
                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
        }
    }

    // insert by path should ensure the fcb is not already in the tree
    result = InsertByPath(parentDcb, &filePath.String, &dirEntInfo, VolumeDeviceObject, &desiredNode);

    if (!NT_SUCCESS(result))
    {
        ExReleaseResourceLite(vcb->Header.Resource);

        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }
        
        return result;
    }

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                result = OpenExistingDcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PDCB)desiredNode, VolumeDeviceObject);
                
                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                result = BlorgOpenExistingFcbExclusive(Irp, fileObject, desiredAccess, shareAccess, (PFCB)desiredNode);
                
                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }
                
                return result;
            }
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    if (filePath.IsAllocated)
    {
        ExFreePool(filePath.String.Buffer);
    }
    
    // We should never reach here.
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

    BOOLEAN topLevel = IsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            BlorgSetupIrpContext(Irp, TRUE);
            result = BlorgVolumeCreate(Irp, irpSp, DeviceObject);
            if (STATUS_PENDING != result)
            {
                CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            result = BlorgDiskCreate(Irp);
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            result = BlorgFileSystemCreate(Irp);
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
    }
    FsRtlExitFileSystem();

    if (topLevel)
    {
        IoSetTopLevelIrp(NULL);
    }

    return result;
}