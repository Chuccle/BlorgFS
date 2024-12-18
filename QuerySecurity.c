#include "Driver.h"

NTSTATUS BlorgQuerySecurity(PDEVICE_OBJECT pDeviceObject, PIRP pIrp) 
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    //PIO_STACK_LOCATION IoStackLocation = IoGetCurrentIrpStackLocation(pIrp);

    pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    pIrp->IoStatus.Information = 0;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}
