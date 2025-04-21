#include "Driver.h"


static NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IrpSp);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    //BLORGFS_PRINT("BlorgVolumeRead...\n");
    //BLORGFS_PRINT(" Irp                    = %p\n", Irp);
    //BLORGFS_PRINT(" ->ByteCount           = %8lx\n", IrpSp->Parameters.Read.Length);
    //BLORGFS_PRINT(" ->ByteOffset.Quadpart = %llx\n", IrpSp->Parameters.Read.ByteOffset.QuadPart);

    //ULONG bytesLength = IrpSp->Parameters.Read.Length;
    //ULONGLONG startingByte = IrpSp->Parameters.Read.ByteOffset.QuadPart;

    //BOOLEAN synchronousIo = BooleanFlagOn(IrpSp->FileObject->Flags, FO_SYNCHRONOUS_IO);

    //if (0 == bytesLength)
    //{
    //    Irp->IoStatus.Information = 0;
    //    return STATUS_SUCCESS;
    //}

    //PFCB fcb = IrpSp->FileObject->FsContext;

    //switch GET_NODE_TYPE(fcb)
    //{
    //    case BLORGFS_FCB_SIGNATURE:
    //    {
    //        break;
    //    }
    //    case BLORGFS_VCB_SIGNATURE:
    //    {
    //        // for now, we only support reading from ordinary files
    //        Irp->IoStatus.Information = 0;
    //        return STATUS_INVALID_PARAMETER;
    //    }
    //    default:
    //    {
    //        BLORGFS_PRINT("BlorgVolumeRead: Invalid node type\n");
    //        Irp->IoStatus.Information = 0;
    //        return STATUS_INVALID_PARAMETER;
    //    }
    //}

    //if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) && BooleanFlagOn(Irp->Flags, IRP_NOCACHE)
    //    &&
    //    IrpSp->FileObject->SectionObjectPointer->DataSectionObject)
    //{
    //    IO_STATUS_BLOCK ioStatus = { 0 };

    //    CcFlushCache(IrpSp->FileObject->SectionObjectPointer,
    //        &startingByte,
    //        bytesLength,
    //        &ioStatus);


    //    if (!NT_SUCCESS(ioStatus.Status))
    //    {
    //        return ioStatus.Status;
    //    }
    //}

    //if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
    //{
    //    result = FsRtlCheckOplock(fcb->Header.Oplock,
    //        Irp,
    //        IrpContext,
    //        FatOplockComplete,
    //        FatPrePostIrp);

    //    if (!NT_SUCCESS(result))
    //    {

    //        OplockPostIrp = TRUE;
    //        PostIrp = TRUE;
    //        try_return(NOTHING);
    //    }

    //    //
    //    //  Reset the flag indicating if Fast I/O is possible since the oplock
    //    //  check could have broken existing (conflicting) oplocks.
    //    //

    //    fcb->Header.IsFastIoPossible = FatIsFastIoPossible(fcb);

    //    //
    //    // We have to check for read access according to the current
    //    // state of the file locks, and set FileSize from the Fcb.
    //    //

    //    if (!FsRtlCheckLockForReadAccess(&fcb->FileLock, Irp))
    //    {
    //        return STATUS_FILE_LOCK_CONFLICT;
    //    }
    //}

    ////
    //// HANDLE THE NON-CACHED CASE
    ////

    //if (BooleanFlagOn(Irp->Flags, IRP_NOCACHE))
    //{

    //    BLORGFS_PRINT("Non cached read.\n");

    //    // 
    //    // Do we want to align the read to the remote cluster size?
    //    //

    //    //
    //    //  Do the read here
    //    //


    //}

    ////
    //// HANDLE CACHED CASE
    ////

    //else
    //{

    //   //
    //   // We delay setting up the file cache until now, in case the
    //   // caller never does any I/O to the file, and thus
    //   // FileObject->PrivateCacheMap == NULL.
    //   //

    //    if (!IrpSp->FileObject->PrivateCacheMap)
    //    {

    //        BLORGFS_PRINT("Initialize cache mapping.\n");

    //        //
    //        //  Get the file allocation size, and if it is less than
    //        //  the file size, raise file corrupt error.
    //        //

    //        if (fcb->Header.AllocationSize.QuadPart == FCB_LOOKUP_ALLOCATIONSIZE_HINT)
    //        {

    //            FatLookupFileAllocationSize(IrpContext, fcb);
    //        }

    //        if (fileSize > fcb->Header.AllocationSize.QuadPart)
    //        {

    //            FatPopUpFileCorrupt(IrpContext, fcb);

    //            FatRaiseStatus(IrpContext, STATUS_FILE_CORRUPT_ERROR);
    //        }

    //        //
    //        //  Now initialize the cache map.
    //        //

    //        CcInitializeCacheMap(IrpSp->FileObject, (PCC_FILE_SIZES)&fcb->Header.AllocationSize, FALSE, /**/, fcb);

    //        CcSetReadAheadGranularity(IrpSp->FileObject, READ_AHEAD_GRANULARITY);
    //    }


    //    //
    //    // DO A NORMAL CACHED READ, if the MDL bit is not set,
    //    //

    //    BLORGFS_PRINT("Cached read.\n");

    //    if (!FlagOn(IrpSp->MinorFunction, IRP_MN_MDL))
    //    {

    //        //
    //        //  Get hold of the user's buffer.
    //        //

    //        PVOID systemBuffer;
    //        result  = MapUserBuffer(Irp, &systemBuffer);

    //        if (!NT_SUCCESS(result))
    //        {
    //            BLORGFS_PRINT("Cached Read: MapUserBuffer failed\n");
    //            return result;
    //        }

    //        //
    //        // Now try to do the copy.
    //        //

    //        __try
    //        {
    //            if (!CcCopyReadEx(IrpSp->FileObject,
    //                &startingByte,
    //                byteCount,
    //                Wait,
    //                systemBuffer,
    //                &Irp->IoStatus,
    //                Irp->Tail.Overlay.Thread))
    //            {
    //                BLORGFS_PRINT("Cached Read could not wait\n");

    //                try_return(PostIrp = TRUE);
    //            }
    //        }
    //        __except (EXCEPTION_EXECUTE_HANDLER)
    //        {
    //            BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());

    //            

    //            try_return(PostIrp = TRUE);
    //        }
    //        
    //        result = Irp->IoStatus.Status;

    //        NT_ASSERT(NT_SUCCESS(result));
    //        
    //    }
    //        //
    //        //  HANDLE A MDL READ
    //        //

    //    else
    //    {

    //        BLORGFS_PRINT("MDL read.\n");

    //        __try
    //        {
    //            CcMdlRead(IrpSp->FileObject,
    //                &startingByte,
    //                ByteCount,
    //                &Irp->MdlAddress,
    //                &Irp->IoStatus);
    //        }
    //        __except (EXCEPTION_EXECUTE_HANDLER)
    //        {
    //            BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());


    //            try_return(PostIrp = TRUE);
    //        }

    //        result = Irp->IoStatus.Status;
    //        NT_ASSERT(NT_SUCCESS(result));

    //    }
    //}

    return result;
}

NTSTATUS BlorgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeRead(Irp, irpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskRead(pIrp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}