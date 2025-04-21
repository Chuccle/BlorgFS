#include "Driver.h"

static NTSTATUS BlorgVolumeClose(PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PVCB vcb = GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb;

    switch GET_NODE_TYPE(fileObject->FsContext)
    {
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            BlorgFreeFileContext(fileObject->FsContext2, VolumeDeviceObject);
            PDCB dcb = fileObject->FsContext;
            InterlockedDecrement64(&dcb->RefCount);

            return STATUS_SUCCESS;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            BlorgFreeFileContext(fileObject->FsContext2, VolumeDeviceObject);
            PDCB dcb = fileObject->FsContext;
            ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);
            dcb->RefCount--;

            break;
        }
        case BLORGFS_FCB_SIGNATURE:
        {
            PFCB fcb = fileObject->FsContext;
            ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);
            fcb->RefCount--;

            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            InterlockedDecrement64(&vcb->RefCount);
            
            return STATUS_SUCCESS;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeCleanup: Unknown Node type\n");
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    PCOMMON_CONTEXT node = fileObject->FsContext;

    if (((BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(node)) &&
        (0 == node->RefCount))
        ||
        (BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(node)) &&
        (IsListEmpty(&((PDCB)node)->ChildrenList)) &&
        (0 == node->RefCount))
    {
        PDCB parentDcb = node->ParentDcb;

        BlorgFreeFileContext(node, VolumeDeviceObject);

        while ((BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(parentDcb)) &&
               (IsListEmpty(&parentDcb->ChildrenList)) &&
               (0 == parentDcb->RefCount))
        {
            PDCB currentDcb = parentDcb;
            parentDcb = currentDcb->ParentDcb;

            BlorgFreeFileContext(currentDcb, VolumeDeviceObject);
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    return STATUS_SUCCESS;
}

NTSTATUS BlorgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeClose(irpSp, DeviceObject);
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
    FsRtlExitFileSystem();
    
    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}