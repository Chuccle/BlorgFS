#include "Driver.h"

//  Small IRP utility helpers shared across dispatch routines.

//
// Resolves the system-address-space pointer for an IRP's I/O buffer. If
// there is no Mdl, the IRP must be in the Fsd, so the UserBuffer field can
// be returned directly; otherwise the Mdl is mapped for safe access.
//
NTSTATUS MapUserBuffer(PIRP Irp, PVOID* Address)
{
    *Address = NULL;

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
