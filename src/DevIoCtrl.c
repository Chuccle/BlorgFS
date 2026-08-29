#include "Driver.h"
#include "Socket.h"
#include "TlsHandshake.h"
#include <mountdev.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntddvol.h>

//
//  IRP_MJ_DEVICE_CONTROL handling for the VDO/DDO/FSDO: synthetic disk
//  geometry and MOUNTDEV identity IOCTLs so the volume mounts and image
//  activation succeeds, plus the FSDO's TLS-pin-update IOCTL.
//

//
//  Synthetic physical-disk number reported for this volume. Arbitrary but
//  must match between IOCTL_STORAGE_GET_DEVICE_NUMBER and
//  IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS. Chosen high to avoid colliding
//  with real disks.
//
#define BLORGFS_SYNTHETIC_DISK_NUMBER  0x42

//
//  Synthetic backing-disk geometry (~1 TB), computed from the same
//  components reported via IOCTL_DISK_GET_DRIVE_GEOMETRY so the two IOCTLs
//  stay consistent.
//
#define BLORGFS_DISK_CYLINDERS            0x20000ULL
#define BLORGFS_DISK_TRACKS_PER_CYLINDER  255ULL
#define BLORGFS_DISK_SECTORS_PER_TRACK    63ULL
#define BLORGFS_DISK_BYTES_PER_SECTOR     512ULL
#define BLORGFS_SYNTHETIC_DISK_SIZE       (BLORGFS_DISK_CYLINDERS *      \
                                           BLORGFS_DISK_TRACKS_PER_CYLINDER * \
                                           BLORGFS_DISK_SECTORS_PER_TRACK *   \
                                           BLORGFS_DISK_BYTES_PER_SECTOR)

//
//  Pushes a new TLS certificate pin (32-byte SHA-256 of the leaf's DER
//  SubjectPublicKeyInfo -- see BlorgTlsSetPin/BlorgTlsCheckPin in TlsHandshake.c)
//  without a driver reload. 0x800 is the first function code in
//  Microsoft's reserved-for-vendor-use range. METHOD_BUFFERED: fixed
//  32-byte payload. FILE_WRITE_ACCESS: this changes what the driver
//  trusts, so it requires the same access an admin-only write would;
//  also enforced by the FSDO device SDDL (BLORGFS_FSDO_DEVICE_SDDL_STRING
//  in Driver.h), which grants World no write access. Device type is
//  FILE_DEVICE_UNKNOWN, not FILE_DEVICE_FILE_SYSTEM: the latter makes the
//  I/O manager route the request as IRP_MJ_FILE_SYSTEM_CONTROL instead of
//  the IRP_MJ_DEVICE_CONTROL this is handled under, so it would never
//  reach DevIoCtrlFsdo at all (see the same note on the
//  statistics IOCTLs in Statistics.h).
//
#define IOCTL_BLORGFS_SET_TLS_PIN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

//
//  IRP_MJ_DEVICE_CONTROL handler for the FSDO: the vendor IOCTLs --
//  IOCTL_BLORGFS_SET_TLS_PIN to push a new cert pin at runtime, and the
//  statistics query/reset pair (Statistics.h) the perf harness samples.
//  Reachable from usermode as \\.\BlorgFS via the symbolic link
//  DriverEntry publishes.
//
//  The statistics query rejects a caller whose BLORGFS_STATISTICS_RESPONSE
//  does not match this driver's: the counter block is append-only, but a
//  harness built against a different revision would still read the wrong
//  fields out of the tail, and silently-wrong performance numbers are
//  worse than none. Both size and version are checked, since a field
//  reordering can leave the size identical.
//
//  Those two gate fields arrive as *input*, so the query validates
//  InputBufferLength as well as OutputBufferLength. METHOD_BUFFERED gives
//  one system buffer sized max(in, out) with only the first
//  InputBufferLength bytes filled in, so a caller passing an output buffer
//  and no input -- the natural shape for something named "query", and legal
//  at the Win32 layer -- would otherwise have its revision decided by
//  whatever the uninitialized tail of that buffer happened to hold. The
//  buffer is genuinely large enough either way (OutputBufferLength is
//  checked first), so this is a determinism fix rather than a bounds one:
//  a revision that cannot be read is rejected as a bad parameter instead of
//  being guessed at.
//
static NTSTATUS DevIoCtrlFsdo(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_BLORGFS_SET_TLS_PIN:
        {
            ULONG inLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (inLength != TLS_HASH_LEN)
            {
                Irp->IoStatus.Information = 0;
                return STATUS_INVALID_PARAMETER;
            }

            Irp->IoStatus.Information = 0;
            return BlorgTlsSetPin(C_CAST(const UCHAR*, Irp->AssociatedIrp.SystemBuffer));
        }

        case IOCTL_BLORGFS_QUERY_STATISTICS:
        {
            ULONG inLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            ULONG outLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

            Irp->IoStatus.Information = 0;

            if (outLength < sizeof(BLORGFS_STATISTICS_RESPONSE))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            if (inLength < sizeof(BLORGFS_STATISTICS_RESPONSE))
            {
                return STATUS_INVALID_PARAMETER;
            }

            PBLORGFS_STATISTICS_RESPONSE response = Irp->AssociatedIrp.SystemBuffer;

            if (BLORGFS_STATISTICS_VERSION != response->Version ||
                sizeof(BLORGFS_STATISTICS_RESPONSE) != response->SizeOfStruct)
            {
                return STATUS_REVISION_MISMATCH;
            }

            BlorgStatisticsQuery(response);

            Irp->IoStatus.Information = sizeof(BLORGFS_STATISTICS_RESPONSE);
            return STATUS_SUCCESS;
        }

        case IOCTL_BLORGFS_RESET_STATISTICS:
        {
            BlorgStatisticsReset();

            Irp->IoStatus.Information = 0;
            return STATUS_SUCCESS;
        }
        default:
        {
            BLORGFS_LOG("UNHANDLED FSDO DeviceControl ioctl=0x%08x -> STATUS_INVALID_DEVICE_REQUEST\n",
                IrpSp->Parameters.DeviceIoControl.IoControlCode);
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
}

//
//  IRP_MJ_DEVICE_CONTROL handler for the DDO: answers the MOUNTDEV identity
//  IOCTLs and the synthetic disk-geometry/device-number/volume-extents
//  IOCTLs that mount and process/image-activation code paths probe for.
//  Each device must identify itself with its OWN name for
//  IOCTL_MOUNTDEV_QUERY_DEVICE_NAME: the mount manager follows B: -> DDO and
//  asks the DDO who it is, and answering with the VDO's name makes it
//  resolve the volume to a device that doesn't match, so the volume lookup
//  fails with "cannot find the file specified". IOCTL_MOUNTDEV_QUERY_UNIQUE_ID
//  needs only a stable, volume-unique blob -- a fixed string is fine since
//  BlorgFS exposes exactly one volume (B:). Suggested link name is B: with
//  UseOnlyIfThereAreNoOtherLinks so the mount manager leaves any existing B:
//  (fallback symlink or user assignment) alone rather than fighting over it.
//  LINK_CREATED/LINK_DELETED notifications require no per-link state, so
//  they are just acknowledged -- returning an error would make the manager
//  treat registration as failed. B: presents as a local fixed disk
//  (FILE_DEVICE_DISK), so apps doing free-space/disk-size checks before
//  launching probe IOCTL_DISK_GET_DRIVE_GEOMETRY; rejecting it surfaces to
//  user mode as ERROR_INVALID_FUNCTION and aborts the launch, so a
//  synthetic-but-self-consistent geometry is reported for a large fixed
//  disk, generous enough that space checks pass. IOCTL_STORAGE_GET_DEVICE_NUMBER
//  and IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS are both issued during process
//  creation as the loader maps the image's volume to a backing storage
//  device to establish the new process's image identity; rejecting either
//  makes NtCreateUserProcess fail with STATUS_INVALID_DEVICE_REQUEST
//  ("Incorrect function") even though every file operation and the image
//  section succeed, so both report a synthetic disk number/extent kept
//  consistent with each other.
//
static NTSTATUS DevIoCtrlDisk(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    ULONG outLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
        {
            UNICODE_STRING ddoName = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
            UNICODE_STRING vdoName = RTL_CONSTANT_STRING(BLORGFS_VDO_STRING);
            UNICODE_STRING deviceName =
                (BlorgDeviceDisk == BlorgDeviceKind(DeviceObject)) ? ddoName : vdoName;

            if (outLength < sizeof(MOUNTDEV_NAME))
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
                return STATUS_BUFFER_TOO_SMALL;
            }

            PMOUNTDEV_NAME name = Irp->AssociatedIrp.SystemBuffer;
            name->NameLength = deviceName.Length;

            if (outLength < UFIELD_OFFSET(MOUNTDEV_NAME, Name) + deviceName.Length)
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
                return STATUS_BUFFER_OVERFLOW;
            }

            RtlCopyMemory(name->Name, deviceName.Buffer, deviceName.Length);
            Irp->IoStatus.Information = UFIELD_OFFSET(MOUNTDEV_NAME, Name) + deviceName.Length;
            return STATUS_SUCCESS;
        }
        case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
        {
            static const WCHAR uniqueId[] = L"BlorgFS\\BlorgVolume";
            USHORT uniqueIdLength = sizeof(uniqueId) - sizeof(WCHAR);

            if (outLength < sizeof(MOUNTDEV_UNIQUE_ID))
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
                return STATUS_BUFFER_TOO_SMALL;
            }

            PMOUNTDEV_UNIQUE_ID id = Irp->AssociatedIrp.SystemBuffer;
            id->UniqueIdLength = uniqueIdLength;

            if (outLength < UFIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + uniqueIdLength)
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
                return STATUS_BUFFER_OVERFLOW;
            }

            RtlCopyMemory(id->UniqueId, uniqueId, uniqueIdLength);
            Irp->IoStatus.Information = UFIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + uniqueIdLength;
            return STATUS_SUCCESS;
        }
        case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
        {
            UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\B:");

            if (outLength < sizeof(MOUNTDEV_SUGGESTED_LINK_NAME))
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_SUGGESTED_LINK_NAME);
                return STATUS_BUFFER_TOO_SMALL;
            }

            PMOUNTDEV_SUGGESTED_LINK_NAME suggested = Irp->AssociatedIrp.SystemBuffer;
            suggested->UseOnlyIfThereAreNoOtherLinks = TRUE;
            suggested->NameLength = linkName.Length;

            if (outLength < UFIELD_OFFSET(MOUNTDEV_SUGGESTED_LINK_NAME, Name) + linkName.Length)
            {
                Irp->IoStatus.Information = sizeof(MOUNTDEV_SUGGESTED_LINK_NAME);
                return STATUS_BUFFER_OVERFLOW;
            }

            RtlCopyMemory(suggested->Name, linkName.Buffer, linkName.Length);
            Irp->IoStatus.Information = UFIELD_OFFSET(MOUNTDEV_SUGGESTED_LINK_NAME, Name) + linkName.Length;
            return STATUS_SUCCESS;
        }
        case IOCTL_MOUNTDEV_LINK_CREATED:
        case IOCTL_MOUNTDEV_LINK_DELETED:
        {
            return STATUS_SUCCESS;
        }
        case IOCTL_DISK_GET_DRIVE_GEOMETRY:
        {
            if (outLength < sizeof(DISK_GEOMETRY))
            {
                Irp->IoStatus.Information = 0;
                return STATUS_BUFFER_TOO_SMALL;
            }

            PDISK_GEOMETRY geometry = Irp->AssociatedIrp.SystemBuffer;
            geometry->Cylinders.QuadPart = BLORGFS_DISK_CYLINDERS;
            geometry->MediaType = FixedMedia;
            geometry->TracksPerCylinder = BLORGFS_DISK_TRACKS_PER_CYLINDER;
            geometry->SectorsPerTrack = BLORGFS_DISK_SECTORS_PER_TRACK;
            geometry->BytesPerSector = BLORGFS_DISK_BYTES_PER_SECTOR;

            Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
            return STATUS_SUCCESS;
        }
        case IOCTL_STORAGE_GET_DEVICE_NUMBER:
        {
            if (outLength < sizeof(STORAGE_DEVICE_NUMBER))
            {
                Irp->IoStatus.Information = 0;
                return STATUS_BUFFER_TOO_SMALL;
            }

            PSTORAGE_DEVICE_NUMBER deviceNumber = Irp->AssociatedIrp.SystemBuffer;
            deviceNumber->DeviceType = FILE_DEVICE_DISK;
            deviceNumber->DeviceNumber = BLORGFS_SYNTHETIC_DISK_NUMBER;
            deviceNumber->PartitionNumber = 1;

            Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);
            return STATUS_SUCCESS;
        }
        case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
        {
            if (outLength < sizeof(VOLUME_DISK_EXTENTS))
            {
                if (outLength >= UFIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents))
                {
                    PVOLUME_DISK_EXTENTS header = Irp->AssociatedIrp.SystemBuffer;
                    header->NumberOfDiskExtents = 1;
                    Irp->IoStatus.Information = FIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents);
                    return STATUS_BUFFER_OVERFLOW;
                }

                Irp->IoStatus.Information = 0;
                return STATUS_BUFFER_TOO_SMALL;
            }

            PVOLUME_DISK_EXTENTS extents = Irp->AssociatedIrp.SystemBuffer;
            extents->NumberOfDiskExtents = 1;
            extents->Extents[0].DiskNumber = BLORGFS_SYNTHETIC_DISK_NUMBER;
            extents->Extents[0].StartingOffset.QuadPart = 0;
            extents->Extents[0].ExtentLength.QuadPart = BLORGFS_SYNTHETIC_DISK_SIZE;

            Irp->IoStatus.Information = sizeof(VOLUME_DISK_EXTENTS);
            return STATUS_SUCCESS;
        }
        default:
        {
            BLORGFS_LOG("UNHANDLED DeviceControl ioctl=0x%08x -> STATUS_INVALID_DEVICE_REQUEST\n",
                IrpSp->Parameters.DeviceIoControl.IoControlCode);
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
}

//
//  IRP_MJ_DEVICE_CONTROL dispatch entry: routes to the DDO or FSDO handler
//  by device type and completes the IRP; the VDO has no IOCTLs of its own
//  (falls through to STATUS_INVALID_DEVICE_REQUEST). The DDO is the
//  synthetic "disk" the volume sits on, and during process creation (and
//  mount-manager registration) the kernel resolves the volume's backing
//  disk and queries it directly, so the disk answers the identity and
//  MOUNTDEV IOCTLs alongside the volume.
//
NTSTATUS BlorgDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            break;
        }
        case BlorgDeviceDisk:
        {
            result = DevIoCtrlDisk(DeviceObject, Irp, irpSp);
            break;
        }
        case BlorgDeviceFileSystem:
        {
            result = DevIoCtrlFsdo(Irp, irpSp);
            break;
        }
    }
    FsRtlExitFileSystem();

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}