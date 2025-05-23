#include "Driver.h"

NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp, PIRP_CONTEXT IrpContext)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BLORGFS_PRINT("BlorgVolumeRead...\n");
    BLORGFS_PRINT(" Irp                    = %p\n", Irp);
    BLORGFS_PRINT(" ->ByteCount           = %8lx\n", IrpSp->Parameters.Read.Length);
    BLORGFS_PRINT(" ->ByteOffset.Quadpart = %llx\n", IrpSp->Parameters.Read.ByteOffset.QuadPart);

    ULONG bytesLength = IrpSp->Parameters.Read.Length;
    ULONG realLength = bytesLength;
    LARGE_INTEGER startingByte = IrpSp->Parameters.Read.ByteOffset;

    if (0 == bytesLength)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    PFCB fcb = IrpSp->FileObject->FsContext;

    switch GET_NODE_TYPE(fcb)
    {
        case BLORGFS_FCB_SIGNATURE:
        {
            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            // for now, we only support reading from ordinary files
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_PARAMETER;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeRead: Invalid node type\n");
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) && 
        BooleanFlagOn(Irp->Flags, IRP_NOCACHE) &&
        IrpSp->FileObject->SectionObjectPointer->DataSectionObject)
    {
        IO_STATUS_BLOCK ioStatus = { 0 };

        CcFlushCache(IrpSp->FileObject->SectionObjectPointer,
            &startingByte,
            bytesLength,
            &ioStatus);

        if (!NT_SUCCESS(ioStatus.Status))
        {
            return ioStatus.Status;
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        result = FsRtlCheckOplock(fcb->Header.Oplock,
            Irp,
            IrpContext,
            OplockComplete,
            PrePostIrp);

        if (STATUS_SUCCESS != result)
        {
            //
            //  Don't send irp to the fsp but also don't 
            //  complete it if cancelled or pending!
            //
            
            return result;
        }

        //
        //  Reset the flag indicating if Fast I/O is possible since the oplock
        //  check could have broken existing (conflicting) oplocks.
        //

        fcb->Header.IsFastIoPossible = FsRtlOplockIsFastIoPossible(&fcb->Header.Oplock);

        //
        //  We have to check for read access according to the current
        //  state of the file locks, and set FileSize from the Fcb.
        //

        if (!FsRtlCheckLockForReadAccess(&fcb->FileLock, Irp))
        {
            return STATUS_FILE_LOCK_CONFLICT;
        }
    }

    //
    // HANDLE THE NON-CACHED CASE
    //

    if (BooleanFlagOn(Irp->Flags, IRP_NOCACHE))
    {
        BLORGFS_PRINT("Non cached read.\n");

        PVOID systemBuffer = (!Irp->MdlAddress) ? Irp->UserBuffer : MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

        if (!systemBuffer)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // 
        //  Do we want to align the read to the remote cluster size?
        //

        if (startingByte.QuadPart >= fcb->Header.FileSize.QuadPart)
        {
            BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
                fcb->Header.AllocationSize.QuadPart,
                startingByte.QuadPart,
                bytesLength);

            return STATUS_INVALID_PARAMETER;
        } 
        else if(startingByte.QuadPart + bytesLength > fcb->Header.FileSize.QuadPart)
        { 
            BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
                fcb->Header.AllocationSize.QuadPart,
                startingByte.QuadPart,
                bytesLength);

            ULONG trimLength = (ULONG)((startingByte.QuadPart + bytesLength) - fcb->Header.AllocationSize.QuadPart);

            realLength = bytesLength - trimLength;
        }

        //
        //  Do the read here
        //
        HTTP_FILE_BUFFER fileBuffer;

        result = GetHttpFile(&fcb->FullPath, startingByte.QuadPart, realLength, &fileBuffer);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("GetHttpFile failure: %8lx\n", result);
            return result;
        }

        __try
        {
            if (!Irp->MdlAddress && UserMode == Irp->RequestorMode)
            {
                ProbeForRead(Irp->UserBuffer, IrpSp->Parameters.QueryDirectory.Length, sizeof(UCHAR));
            }
            
            RtlCopyMemory(systemBuffer, fileBuffer.BodyBuffer, bytesLength);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            BLORGFS_PRINT("Read buffer copy exception: %8lx\n", GetExceptionCode());
            FreeHttpFile(&fileBuffer);
            return GetExceptionCode();
        }

        FreeHttpFile(&fileBuffer);
    }

    //
    // HANDLE CACHED CASE
    //

    else
    {
       //
       // We delay setting up the file cache until now, in case the
       // caller never does any I/O to the file, and thus
       // FileObject->PrivateCacheMap == NULL.
       //

        if (!IrpSp->FileObject->PrivateCacheMap)
        {
            BLORGFS_PRINT("Initialize cache mapping.\n");

            //
            //  Now initialize the cache map.
            //

            CcInitializeCacheMap(IrpSp->FileObject, (PCC_FILE_SIZES)&fcb->Header.AllocationSize, FALSE, &global.CacheManagerCallbacks, fcb);

            // CcSetReadAheadGranularity(IrpSp->FileObject, READ_AHEAD_GRANULARITY);
        }

        //
        //  Trim ReadLength; the cache manager does not tolerate reads beyond file size
        //

        if (startingByte.QuadPart >= fcb->Header.FileSize.QuadPart)
        {
            BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
                fcb->Header.AllocationSize.QuadPart,
                startingByte.QuadPart,
                bytesLength);

            return STATUS_INVALID_PARAMETER;
        }
        else if (startingByte.QuadPart + bytesLength > fcb->Header.FileSize.QuadPart)
        {
            BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
                fcb->Header.AllocationSize.QuadPart,
                startingByte.QuadPart,
                bytesLength);

            ULONG trimLength = (ULONG)((startingByte.QuadPart + bytesLength) - fcb->Header.AllocationSize.QuadPart);

            realLength = bytesLength - trimLength;
        }

        //
        //  DO A NORMAL CACHED READ, if the MDL bit is not set,
        //

        BLORGFS_PRINT("Cached read.\n");

        if (!FlagOn(IrpSp->MinorFunction, IRP_MN_MDL))
        {
            //
            //  Get hold of the user's buffer.
            //

            PVOID systemBuffer = (!Irp->MdlAddress) ? Irp->UserBuffer : MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
            
            if (!systemBuffer)
            {
               return STATUS_INSUFFICIENT_RESOURCES;
            }

            //
            //  Now try to do the copy.
            //

            __try
            {
                if (!CcCopyReadEx(IrpSp->FileObject,
                    &startingByte,
                    realLength,
                    BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT),
                    systemBuffer,
                    &Irp->IoStatus,
                    Irp->Tail.Overlay.Thread))
                {
                    BLORGFS_PRINT("Cached Read could not wait\n");
                    return FsdPostRequest(IrpContext, Irp, IrpSp);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());
                return GetExceptionCode();
            }
            
            result = Irp->IoStatus.Status;

            NT_ASSERT(NT_SUCCESS(result));
        }
            
        //  
        //  HANDLE A MDL READ
        //

        else
        {
            BLORGFS_PRINT("MDL read.\n");

            __try
            {
                CcMdlRead(IrpSp->FileObject,
                    &startingByte,
                    realLength,
                    &Irp->MdlAddress,
                    &Irp->IoStatus);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());
                return GetExceptionCode();
            }

            result = Irp->IoStatus.Status;
            NT_ASSERT(NT_SUCCESS(result));
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        //
        //  If the file was opened for Synchronous IO, update the current
        //  file position.
        //
        
        if (BooleanFlagOn(IrpSp->FileObject->Flags, FO_SYNCHRONOUS_IO))
        {
            IrpSp->FileObject->CurrentByteOffset.QuadPart = startingByte.QuadPart + Irp->IoStatus.Information;
        }

        //
        //  If this was not PagingIo, mark that the last access
        //  time on the dirent needs to be updated on close.
        //

        if (NT_SUCCESS(result))
        {
            SetFlag(IrpSp->FileObject->Flags, FO_FILE_FAST_IO_READ);
        }
    }

    return result;
}

NTSTATUS BlorgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    //PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = IsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            //PIRP_CONTEXT irpContext = BlorgCreateIrpContext(Irp, IoIsOperationSynchronous(Irp));
            //result = BlorgVolumeRead(Irp, irpSp, irpContext);
            //if (STATUS_PENDING != result || STATUS_CANCELLED != result)
            //{
            //    CompleteRequest(irpContext, Irp, result);
            //}
            CompleteRequest(NULL, Irp, result);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskRead(pIrp);
            CompleteRequest(NULL, Irp, result);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            CompleteRequest(NULL, Irp, result);
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