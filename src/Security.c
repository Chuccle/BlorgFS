#include "Driver.h"

//
//  Global security descriptor setup/teardown, and IRP_MJ_QUERY_SECURITY /
//  IRP_MJ_SET_SECURITY handling. All files/directories share one
//  self-relative SD; set-security is unimplemented.
//

NTSTATUS BlorgInitializeSecurityDescriptor(VOID)
{
    SECURITY_DESCRIPTOR absolute;

    NTSTATUS status = RtlCreateSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    RtlSetDaclSecurityDescriptor(&absolute, TRUE, NULL, FALSE);

    status = RtlSetOwnerSecurityDescriptor(&absolute, SeExports->SeLocalSystemSid, FALSE);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = RtlSetGroupSecurityDescriptor(&absolute, SeExports->SeLocalSystemSid, FALSE);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ULONG length = 0;
    status = RtlAbsoluteToSelfRelativeSD(&absolute, NULL, &length);

    if (STATUS_BUFFER_TOO_SMALL != status)
    {
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    PSECURITY_DESCRIPTOR selfRelative = ExAllocatePoolZero(NonPagedPoolNx, length, 'CESB');

    if (!selfRelative)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = RtlAbsoluteToSelfRelativeSD(&absolute, selfRelative, &length);

    if (!NT_SUCCESS(status))
    {
        ExFreePool(selfRelative);
        return status;
    }

    global.FileSecurityDescriptor = selfRelative;

    return STATUS_SUCCESS;
}

//
// Frees the shared self-relative security descriptor, if allocated. Called
// at driver unload.
//
VOID BlorgFreeSecurityDescriptor(VOID)
{
    if (global.FileSecurityDescriptor)
    {
        ExFreePool(global.FileSecurityDescriptor);
        global.FileSecurityDescriptor = NULL;
    }
}

//
// Volume IRP_MJ_QUERY_SECURITY handler: returns the requested components of
// the single shared security descriptor into the IRP's user buffer via
// SeQuerySecurityDescriptorInfo, guarded by SEH since the buffer is
// user-supplied.
//
static NTSTATUS BlorgVolumeQuerySecurity(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    SECURITY_INFORMATION securityInformation = IrpSp->Parameters.QuerySecurity.SecurityInformation;
    ULONG length = IrpSp->Parameters.QuerySecurity.Length;
    PSECURITY_DESCRIPTOR objectSd = global.FileSecurityDescriptor;

    NTSTATUS status;

    __try
    {
        status = SeQuerySecurityDescriptorInfo(
            &securityInformation,
            Irp->UserBuffer,
            &length,
            &objectSd);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        length = 0;
    }

    Irp->IoStatus.Information = length;
    return status;
}

//
// IRP_MJ_QUERY_SECURITY dispatch entry point: routes to
// BlorgVolumeQuerySecurity for the volume device object and completes the
// IRP with the result (disk/FS-control device objects are unimplemented).
//
NTSTATUS BlorgQuerySecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            result = BlorgVolumeQuerySecurity(Irp, irpSp);
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

//
// IRP_MJ_SET_SECURITY dispatch entry point: unimplemented for every device
// object type, always completes with STATUS_INVALID_DEVICE_REQUEST.
//
NTSTATUS BlorgSetSecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
