#include "Driver.h"

static inline void QueryBasicInfo(
   const PCOMMON_CONTEXT CommonContext,
    PFILE_BASIC_INFORMATION BasicInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_BASIC_INFORMATION))
    {
        *Length = -1;
        return;
    }

    BasicInfo->CreationTime.QuadPart = CommonContext->CreationTime;
    BasicInfo->LastAccessTime.QuadPart = CommonContext->LastAccessedTime;
    BasicInfo->LastWriteTime.QuadPart = CommonContext->LastModifiedTime;
    BasicInfo->ChangeTime.QuadPart = CommonContext->LastModifiedTime;
    BasicInfo->FileAttributes = (GET_NODE_TYPE(CommonContext) == BLORGFS_FCB_SIGNATURE)
        ? FILE_ATTRIBUTE_NORMAL
        : FILE_ATTRIBUTE_DIRECTORY;

    *Length -= sizeof(FILE_BASIC_INFORMATION);
}

static inline void QueryStandardInfo(
    const PCOMMON_CONTEXT CommonContext,
    PFILE_STANDARD_INFORMATION StandardInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_STANDARD_INFORMATION))
    {
        *Length = -1;
        return;
    }

    StandardInfo->AllocationSize = CommonContext->Header.AllocationSize;
    StandardInfo->EndOfFile = CommonContext->Header.AllocationSize;
    StandardInfo->NumberOfLinks = 1;
    StandardInfo->DeletePending = FALSE;
    StandardInfo->Directory = (GET_NODE_TYPE(CommonContext) == BLORGFS_DCB_SIGNATURE);

    *Length -= sizeof(FILE_STANDARD_INFORMATION);
}

static inline void QueryInternalInfo(
    PFILE_INTERNAL_INFORMATION InternalInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_INTERNAL_INFORMATION))
    {
        *Length = -1;
        return;
    }

    InternalInfo->IndexNumber.QuadPart = 0;
    *Length -= sizeof(FILE_INTERNAL_INFORMATION);
}


static inline void QueryEaInfo(
    PFILE_EA_INFORMATION EaInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_EA_INFORMATION))
    {
        *Length = -1;
        return;
    }

    EaInfo->EaSize = 0;
    *Length -= sizeof(FILE_EA_INFORMATION);
}

static inline void QueryPositionInfo(
    const PFILE_OBJECT FileObject,
    PFILE_POSITION_INFORMATION PositionInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_POSITION_INFORMATION))
    {
        *Length = -1;
        return;
    }

    PositionInfo->CurrentByteOffset = FileObject->CurrentByteOffset;
    *Length -= sizeof(FILE_POSITION_INFORMATION);
}

static inline void QueryNameInfo(
    const PCOMMON_CONTEXT CommonContext,
    PFILE_NAME_INFORMATION NameInfo,
    PLONG Length
)
{
    ULONG requiredSize = sizeof(FILE_NAME_INFORMATION) + CommonContext->FullPath.Length;

    if (*Length < (LONG)requiredSize)
    {
        *Length = -1;
        return;
    }

    NameInfo->FileNameLength = CommonContext->FullPath.Length;
    RtlCopyMemory(NameInfo->FileName,
        CommonContext->FullPath.Buffer,
        NameInfo->FileNameLength);
    *Length -= requiredSize;
    
}


static inline void QueryNetworkInfo(
    const PCOMMON_CONTEXT CommonContext,
    PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
    PLONG Length
)
{
    if (*Length < sizeof(FILE_NETWORK_OPEN_INFORMATION))
    {
        *Length = -1;
        return;
    }

    NetworkInfo->AllocationSize = CommonContext->Header.AllocationSize;
    NetworkInfo->EndOfFile = CommonContext->Header.AllocationSize;
    NetworkInfo->CreationTime.QuadPart = CommonContext->CreationTime;
    NetworkInfo->LastAccessTime.QuadPart = CommonContext->LastAccessedTime;
    NetworkInfo->LastWriteTime.QuadPart = CommonContext->LastModifiedTime;
    NetworkInfo->ChangeTime.QuadPart = CommonContext->LastModifiedTime;
    NetworkInfo->FileAttributes = (GET_NODE_TYPE(CommonContext) == BLORGFS_FCB_SIGNATURE)
        ? FILE_ATTRIBUTE_NORMAL
        : FILE_ATTRIBUTE_DIRECTORY;

    *Length -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
}

static NTSTATUS BlorgVolumeQueryInformation(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    // GetVolumeInformation crashes without this being implemented, come on Windows!
    FILE_INFORMATION_CLASS fileInfoClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    LONG length = IrpSp->Parameters.QueryFile.Length;
    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PCOMMON_CONTEXT commonContext = IrpSp->FileObject->FsContext;

    switch (fileInfoClass)
    {
        case FileAllInformation:
        {
            PFILE_ALL_INFORMATION allInfo = systemBuffer;

            //
            //  Reserve space for the internal sections
            //
            length -= (sizeof(FILE_ACCESS_INFORMATION) +
                sizeof(FILE_MODE_INFORMATION) +
                sizeof(FILE_ALIGNMENT_INFORMATION));

            QueryBasicInfo(commonContext, &allInfo->BasicInformation, &length);
            QueryStandardInfo(commonContext, &allInfo->StandardInformation, &length);
            QueryInternalInfo(&allInfo->InternalInformation, &length);
            QueryEaInfo(&allInfo->EaInformation, &length);
            QueryPositionInfo(fileObject, &allInfo->PositionInformation, &length);
            QueryNameInfo(commonContext, &allInfo->NameInformation, &length);

            break;
        }
        case FileBasicInformation:
        {
            QueryBasicInfo(commonContext, systemBuffer, &length);
            break;
        }
        case FileStandardInformation:
        {
            QueryStandardInfo(commonContext, systemBuffer, &length);
            break;
        }
        case FileInternalInformation:
        {
            QueryInternalInfo(systemBuffer, &length);
            break;
        }
        case FileEaInformation:
        {
            QueryEaInfo(systemBuffer, &length);
            break;
        }
        case FilePositionInformation:
        {
            QueryPositionInfo(fileObject, systemBuffer, &length);
            break;
        }
        case FileNameInformation:
        {
            QueryNameInfo(commonContext, systemBuffer, &length);
            break;
        }
        case FileNormalizedNameInformation:
        {
            QueryNameInfo(commonContext, systemBuffer, &length);
            break;
        }
        case FileNetworkOpenInformation:
        {
            QueryNetworkInfo(commonContext, systemBuffer, &length);
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            length = 0;
            break;
        }
    }

    //
    //  If we overflowed the buffer, set length to 0 and return STATUS_BUFFER_OVERFLOW
    //
    
    if (length < 0)
    {
        result = STATUS_BUFFER_OVERFLOW;
        length = 0;
    }

    Irp->IoStatus.Information = IrpSp->Parameters.QueryFile.Length - length;

    return result;
}

NTSTATUS BlorgQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeQueryInformation(Irp, irpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskQueryInformation(pIrp);
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


NTSTATUS BlorgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    // PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            // result = BlorgVolumeSetInformation(Irp, irpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskSetInformation(Irp);
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