#include "Driver.h"

NTSTATUS MapUserBuffer(PIRP Irp, PVOID* Address)
{
    *Address = NULL;
    //
    // If there is no Mdl, then we must be in the Fsd, and we can simply
    // return the UserBuffer field from the Irp.
    //
    if (!Irp->MdlAddress)
    {
        *Address = Irp->UserBuffer;
        return STATUS_SUCCESS;

    }
    else
    {
        PVOID address = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

        if (!address)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        *Address = address;
        return STATUS_SUCCESS;
    }
}