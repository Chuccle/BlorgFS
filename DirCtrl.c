#include "Driver.h"

static NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    KdBreakPoint();
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (IrpSp->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
        {
            KdBreakPoint();
            BLORGFS_PRINT("FatQueryDirectory...\n");
            BLORGFS_PRINT(" Irp                    = %p\n", Irp);
            BLORGFS_PRINT(" ->Length               = %08lx\n", IrpSp->Parameters.QueryDirectory.Length);
            BLORGFS_PRINT(" ->FileName             = %wZ\n", IrpSp->Parameters.QueryDirectory.FileName);
            BLORGFS_PRINT(" ->FileInformationClass = %08lx\n", IrpSp->Parameters.QueryDirectory.FileInformationClass);
            BLORGFS_PRINT(" ->FileIndex            = %08lx\n", IrpSp->Parameters.QueryDirectory.FileIndex);
            BLORGFS_PRINT(" ->UserBuffer           = %p\n", Irp->AssociatedIrp.SystemBuffer);
            BLORGFS_PRINT(" ->RestartScan          = %08lx\n", FlagOn(IrpSp->Flags, SL_RESTART_SCAN));
            BLORGFS_PRINT(" ->ReturnSingleEntry    = %08lx\n", FlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY));
            BLORGFS_PRINT(" ->IndexSpecified       = %08lx\n", FlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED));

            BOOLEAN restartScan = FlagOn(IrpSp->Flags, SL_RESTART_SCAN);
            //BOOLEAN returnSingleEntry = FlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);
            //BOOLEAN indexSpecified = FlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED);

            PDCB pDcb = IrpSp->FileObject->FsContext;

            switch GET_NODE_TYPE(pDcb)
            {
                case BLORGFS_DCB_SIGNATURE:
                {
                    break;

                }
                case BLORGFS_ROOT_DCB_SIGNATURE:
                {
                    break;
                }
                default:
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Unknown FCB type\n");
                    Irp->IoStatus.Information = 0;
                    return STATUS_INVALID_PARAMETER;

                }
            }

            PCCB pCcb = IrpSp->FileObject->FsContext2;

            if (!pCcb)
            {
                return STATUS_INVALID_PARAMETER;
            }

            BOOLEAN initialQuery = (BOOLEAN)((!pCcb->SearchPattern.Buffer) &&
                !FlagOn(pCcb->Flags, CCB_FLAG_MATCH_ALL));

            if (initialQuery || restartScan)
            {
                ExAcquireResourceExclusiveLite(pDcb->Header.Resource, TRUE);

                //
                // Protect against race condition where CCB has been modified in the time window before being locked by another thread
                //

                if (!restartScan && (pCcb->SearchPattern.Buffer || FlagOn(pCcb->Flags, CCB_FLAG_MATCH_ALL)))
                {
                    initialQuery = FALSE;
                    ExConvertExclusiveToSharedLite(pDcb->Header.Resource);
                }

            }
            else
            {
                ExAcquireResourceSharedLite(pDcb->Header.Resource, TRUE);
            }

            if ((IrpSp->Parameters.QueryDirectory.FileName) && (IrpSp->Parameters.QueryDirectory.FileName->Buffer) && (0 < IrpSp->Parameters.QueryDirectory.FileName->Length))
            {

                //
                // If we're restarting the scan, clear out the pattern in the Ccb and regenerate it.
                //

                if (initialQuery || restartScan)
                {
                    RtlZeroMemory(&pCcb->Flags, sizeof(ULONGLONG));

                    if (pCcb->SearchPattern.Buffer)
                    {
                        ExFreePool(pCcb->SearchPattern.Buffer);
                    }

                    PUNICODE_STRING fileName = IrpSp->Parameters.QueryDirectory.FileName;
                    pCcb->SearchPattern.Buffer = ExAllocatePoolUninitialized(
                        PagedPool,
                        IrpSp->Parameters.QueryDirectory.FileName->Length,
                        'ccb');

                    if (!pCcb->SearchPattern.Buffer)
                    {
                        ExReleaseResourceLite(pDcb->Header.Resource);
                        result = STATUS_INSUFFICIENT_RESOURCES;
                        Irp->IoStatus.Information = 0;
                        break;
                    }

                    pCcb->SearchPattern.Length = fileName->Length;
                    pCcb->SearchPattern.MaximumLength = fileName->Length;
                    RtlCopyMemory(pCcb->SearchPattern.Buffer, fileName->Buffer, fileName->Length);

                    if ((pCcb->SearchPattern.Length == sizeof(WCHAR)) && (pCcb->SearchPattern.Buffer[0] == L'*'))
                    {
                        SetFlag(pCcb->Flags, CCB_FLAG_MATCH_ALL);
                    }

                }
            }
            else
            {
                if (initialQuery || restartScan)
                {
                    RtlZeroMemory(&pCcb->Flags, sizeof(ULONGLONG));

                    if (pCcb->SearchPattern.Buffer)
                    {
                        ExFreePool(pCcb->SearchPattern.Buffer);
                        RtlZeroMemory(&pCcb->SearchPattern, sizeof(UNICODE_STRING));
                    }

                    SetFlag(pCcb->Flags, CCB_FLAG_MATCH_ALL);

                }
            }


            ExReleaseResourceLite(pDcb->Header.Resource);

            switch (IrpSp->Parameters.QueryDirectory.FileInformationClass)
            {
                case FileIdBothDirectoryInformation:
                {
                    break;
                }
                case FileDirectoryInformation:
                {
                    break;
                }
                case FileFullDirectoryInformation:
                {
                    break;
                }
                case FileIdFullDirectoryInformation:
                {
                    break;
                }
                case FileNamesInformation:
                {

                    break;
                }
                case FileBothDirectoryInformation:
                {
                    break;
                }
                default:
                {
                    return STATUS_INVALID_INFO_CLASS;
                }
            }

            result = STATUS_NOT_IMPLEMENTED;
            Irp->IoStatus.Information = 0;
            break;
        }
        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        {
            result = STATUS_NOT_IMPLEMENTED;
            Irp->IoStatus.Information = 0;
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            Irp->IoStatus.Information = 0;
        }
    }

    return result;
}


NTSTATUS BlorgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeDirectoryControl(Irp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskDirectoryControl(pIrp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            break;
        }
    }
    FsRtlExitFileSystem();

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}