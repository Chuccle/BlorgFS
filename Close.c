#include "Driver.h"

static NTSTATUS BlorgVolumeClose(PIO_STACK_LOCATION IrpSp)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    PFILE_OBJECT pFileObject = IrpSp->FileObject;

    switch GET_NODE_TYPE(pFileObject->FsContext)
    {
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            result = STATUS_SUCCESS;

            PDCB pDcb = pFileObject->FsContext;

            InterlockedDecrement64(&pDcb->RefCount);

            BlorgFreeFileContext(pFileObject->FsContext2);

            break;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;

            BlorgFreeFileContext(pFileObject->FsContext2);

            PDCB pDcb = pFileObject->FsContext;

            if (InterlockedDecrement64(&pDcb->RefCount) == 0)
            {
                BlorgFreeFileContext(pDcb);
            }

            break;
        }
        case BLORGFS_FCB_SIGNATURE:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;

            PDCB pFcb = pFileObject->FsContext;

            if (InterlockedDecrement64(&pFcb->RefCount) == 0)
            {
                BlorgFreeFileContext(pFcb);
            }

            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            result = STATUS_SUCCESS;

            PDCB pVcb = pFileObject->FsContext;

            InterlockedDecrement64(&pVcb->RefCount);

            break;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeCleanup: Unknown FCB type\n");
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    return result;
}

NTSTATUS BlorgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeClose(pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskClose(pIrp);
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