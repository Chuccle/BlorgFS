//
// Reads the driver's configuration from <services key>\Parameters at
// DriverEntry, so the backend's cert pin, host and port and the tuning
// settings never need a rebuild to change. Compiled into the sandbox too
// (RegistryModel.c scripts the registry under it).
//
// Every failure is tolerated (no Parameters key, a value missing, of the
// wrong type or size, or malformed) and keeps that setting's default: this
// must never fail driver load, matching BlorgTlsGlobalInit's own "TLS is
// opt-in" policy.
//

#include "Driver.h"
#include "Registry.h"
#include "Socket.h"
#include "TlsHandshake.h"

//
// Reads a single registry value of the expected type into Buffer, failing
// if the stored value doesn't match ExpectedType or exceeds BufferSize.
// infoBuffer's headroom over KEY_VALUE_PARTIAL_INFORMATION's own header is
// the largest value this driver reads, a BLORGFS_REG_DISK_CACHE_PATH_MAX_CHARS
// path, and covers the rest (a DWORD, a 32-byte pin, a short port string,
// or a BLORGFS_REG_HOST_MAX_CHARS hostname) -- not a general-purpose
// arbitrarily-sized read.
//
static NTSTATUS RegistryReadValue(
    HANDLE ParametersKey,
    PCWSTR ValueName,
    ULONG ExpectedType,
    PVOID Buffer,
    ULONG BufferSize,
    PULONG ActualSize)
{
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, ValueName);

    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Alignment;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + BLORGFS_REG_DISK_CACHE_PATH_MAX_CHARS * sizeof(WCHAR)];
    } infoBuffer;
    ULONG resultLength = 0;

    NTSTATUS status = ZwQueryValueKey(
        ParametersKey,
        &valueName,
        KeyValuePartialInformation,
        infoBuffer.Bytes,
        sizeof(infoBuffer),
        &resultLength);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PKEY_VALUE_PARTIAL_INFORMATION info = C_CAST(PKEY_VALUE_PARTIAL_INFORMATION, infoBuffer.Bytes);

    const ULONG headerSize = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);

    if (resultLength < headerSize || resultLength > sizeof(infoBuffer))
    {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (info->Type != ExpectedType || info->DataLength > BufferSize ||
        info->DataLength > resultLength - headerSize ||
        (REG_DWORD == ExpectedType && sizeof(ULONG) != info->DataLength))
    {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    RtlCopyMemory(Buffer, info->Data, info->DataLength);
    *ActualSize = info->DataLength;

    return STATUS_SUCCESS;
}

//
// Accepts a registry-supplied RemotePort only if it is all digits and
// parses to 1..65535. Anything else -- empty, non-numeric, too long, out
// of range -- is rejected so the caller keeps the scheme-default port: a
// garbage port fed to BlorgGetHttpAddrInfo would otherwise surface only as an
// opaque resolve/connect failure at driver load. The 5-character cap is
// "65535"'s length, which also keeps the accumulator far from overflow.
//
static BOOLEAN RegistryValidPortString(const WCHAR* Port, USHORT PortChars)
{
    if (0 == PortChars || PortChars > 5)
    {
        return FALSE;
    }

    ULONG value = 0;

    for (USHORT i = 0; i < PortChars; ++i)
    {
        if (Port[i] < L'0' || Port[i] > L'9')
        {
            return FALSE;
        }

        value = (value * 10) + (Port[i] - L'0');
    }

    return (value >= 1) && (value <= 65535);
}

//
// REG_SZ may omit its terminator. Odd byte lengths, empty values and
// embedded NULs are malformed; leave the caller's counted default intact.
//
static USHORT RegistryStringChars(const WCHAR* Value, ULONG Bytes)
{
    if (Bytes < sizeof(WCHAR) || Bytes % sizeof(WCHAR))
    {
        return 0;
    }

    USHORT chars = C_CAST(USHORT, Bytes / sizeof(WCHAR));

    if (!Value[chars - 1])
    {
        --chars;
    }

    for (USHORT i = 0; i < chars; ++i)
    {
        if (!Value[i])
        {
            return 0;
        }
    }

    return chars;
}

//
// All outputs are counted strings. Respect their supplied capacities even
// when a caller uses a smaller buffer than DriverEntry's declared caps.
//
static VOID RegistryCopyString(PUNICODE_STRING Output, const WCHAR* Value, USHORT Chars)
{
    const USHORT bytes = Chars * sizeof(WCHAR);

    if (0 == Chars || bytes > Output->MaximumLength)
    {
        return;
    }

    RtlCopyMemory(Output->Buffer, Value, bytes);
    Output->Length = bytes;
}

//
// Opens <ServiceRegistryPath>\Parameters and reads every override into the
// globals, PortOut, HostOut and DiskCachePathOut, at PASSIVE_LEVEL. DriverEntry
// seeds the defaults first. String values are taken with or without a
// terminating NUL, since RegSetValueEx stores whatever it is given; an odd
// length, an empty value or an embedded NUL is malformed, and nothing is
// copied past an output's MaximumLength. RemotePort is additionally
// validated as a real port number (RegistryValidPortString).
//
// ReadAheadGranularityKb is read here too, in kilobytes, so Cc's read-ahead
// granularity can be swept without a rebuild and redeploy per point. The
// committed default was measured on eight concurrent streams -- a
// throughput workload -- and never against a latency-shaped one, and
// nothing below 256 KB was ever tried, where FastFat uses 64 KB and Cc's
// own default is a page. Zero means do not call CcSetReadAheadGranularity
// at all, which is the "leave it default" case and cannot be expressed by
// any other value -- and note that Cc's default is PAGE_SIZE, a constant,
// not an adaptive policy, so zero is the smallest setting rather than the
// absence of one.
//
// Anything else is rejected unless it is a power of two of at least
// PAGE_SIZE, which is what ntifs.h states the granularity must be, and its
// bytes fit a ULONG. The value reaches Cc unexamined otherwise, and a
// plausible-looking 100 KB would hand it a number it does not accept; a
// sweep is exactly where someone types one.
//
// DiskCacheMb sizes the disk cache and DiskCachePath, an NT path, names its
// file; DiskCachePathOut is left empty without one, which means the default.
//
VOID BlorgReadRegistryConfig(PUNICODE_STRING ServiceRegistryPath, PUNICODE_STRING PortOut, PUNICODE_STRING HostOut, PUNICODE_STRING DiskCachePathOut)
{
    UNICODE_STRING parametersSuffix = RTL_CONSTANT_STRING(L"\\Parameters");

    UNICODE_STRING parametersPath;
    parametersPath.Length = 0;
    if (ServiceRegistryPath->Length > MAXUSHORT - parametersSuffix.Length - sizeof(WCHAR))
    {
        return;
    }

    parametersPath.MaximumLength = ServiceRegistryPath->Length + parametersSuffix.Length + sizeof(WCHAR);
    parametersPath.Buffer = ExAllocatePoolZero(NonPagedPoolNx, parametersPath.MaximumLength, BLORGFS_REG_TAG);

    if (!parametersPath.Buffer)
    {
        return;
    }

    RtlCopyMemory(parametersPath.Buffer, ServiceRegistryPath->Buffer, ServiceRegistryPath->Length);
    RtlCopyMemory(C_CAST(PUCHAR, parametersPath.Buffer) + ServiceRegistryPath->Length,
        parametersSuffix.Buffer, parametersSuffix.Length);
    parametersPath.Length = ServiceRegistryPath->Length + parametersSuffix.Length;

    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(&objectAttributes, &parametersPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    HANDLE parametersKey;
    NTSTATUS status = ZwOpenKey(&parametersKey, KEY_READ, &objectAttributes);

    ExFreePool(parametersPath.Buffer);

    if (!NT_SUCCESS(status))
    {
        return;
    }

    ULONG tlsEnabledValue = 0;
    ULONG actualSize = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"TlsEnabled", REG_DWORD, &tlsEnabledValue, sizeof(tlsEnabledValue), &actualSize)))
    {
        global.TlsEnabled = (0 != tlsEnabledValue);
    }

    ULONG granularityKb = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ReadAheadGranularityKb", REG_DWORD, &granularityKb, sizeof(granularityKb), &actualSize))
        && granularityKb <= MAXULONG / 1024)
    {
        const ULONG granularity = granularityKb * 1024;

        if (0 == granularity ||
            (granularity >= PAGE_SIZE && 0 == (granularity & (granularity - 1))))
        {
            global.ReadAheadGranularity = granularity;
        }
    }

    ULONG maxGranularityKb = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ReadAheadMaxGranularityKb", REG_DWORD, &maxGranularityKb, sizeof(maxGranularityKb), &actualSize))
        && maxGranularityKb <= MAXULONG / 1024)
    {
        const ULONG maxGranularity = maxGranularityKb * 1024;

        if (maxGranularity >= PAGE_SIZE && 0 == (maxGranularity & (maxGranularity - 1)))
        {
            global.ReadAheadMaxGranularity = maxGranularity;
        }
    }

    ULONG fairBudgetKb = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ReadFairBudgetKb", REG_DWORD, &fairBudgetKb, sizeof(fairBudgetKb), &actualSize))
        && fairBudgetKb <= MAXULONG / 1024)
    {
        global.ReadFairBudget = fairBudgetKb * 1024;
    }

    ULONG adaptValue = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ReadAheadAdapt", REG_DWORD, &adaptValue, sizeof(adaptValue), &actualSize)))
    {
        global.ReadAheadAdapt = (0 != adaptValue);
    }

    ULONG changeFeedValue = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ChangeFeed", REG_DWORD, &changeFeedValue, sizeof(changeFeedValue), &actualSize)))
    {
        global.ChangeFeed = (0 != changeFeedValue);
    }

    ULONG subtreeEntries = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"SubtreeEntries", REG_DWORD, &subtreeEntries, sizeof(subtreeEntries), &actualSize)))
    {
        global.SubtreeEntries = subtreeEntries;
    }

    ULONG diskCacheMb = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"DiskCacheMb", REG_DWORD, &diskCacheMb, sizeof(diskCacheMb), &actualSize)))
    {
        global.DiskCacheMb = diskCacheMb;
    }

    WCHAR diskCachePathValue[BLORGFS_REG_DISK_CACHE_PATH_MAX_CHARS];

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"DiskCachePath", REG_SZ, diskCachePathValue, sizeof(diskCachePathValue), &actualSize)))
    {
        RegistryCopyString(DiskCachePathOut, diskCachePathValue, RegistryStringChars(diskCachePathValue, actualSize));
    }

    ULONG slackGrowthValue = 0;

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"ReadAheadSlackGrowth", REG_DWORD, &slackGrowthValue, sizeof(slackGrowthValue), &actualSize)))
    {
        global.ReadAheadSlackGrowth = (0 != slackGrowthValue);
    }

    UCHAR pinValue[TLS_HASH_LEN];

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"TlsPin", REG_BINARY, pinValue, sizeof(pinValue), &actualSize))
        && TLS_HASH_LEN == actualSize)
    {
        BlorgTlsSetPin(pinValue);
    }

    WCHAR portValue[BLORGFS_REG_PORT_MAX_CHARS];

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"RemotePort", REG_SZ, portValue, sizeof(portValue), &actualSize)))
    {
        USHORT portChars = RegistryStringChars(portValue, actualSize);

        if (RegistryValidPortString(portValue, portChars))
        {
            RegistryCopyString(PortOut, portValue, portChars);
        }
    }

    WCHAR hostValue[BLORGFS_REG_HOST_MAX_CHARS];

    if (NT_SUCCESS(RegistryReadValue(parametersKey, L"RemoteHost", REG_SZ, hostValue, sizeof(hostValue), &actualSize)))
    {
        RegistryCopyString(HostOut, hostValue, RegistryStringChars(hostValue, actualSize));
    }

    ZwClose(parametersKey);
}
