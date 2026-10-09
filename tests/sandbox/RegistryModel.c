//
// One selected Parameters value; all others are missing. This exercises
// the parser's default policy without duplicating its validation rules.
//
#include "SandboxSocket.h"
#include <wchar.h>

static struct
{
    PCWSTR Name;
    const VOID* Data;
    ULONG Type;
    ULONG Length;
    NTSTATUS OpenStatus;
    NTSTATUS QueryStatus;
    ULONG ResultLength;
    ULONG DataLength;
    BOOLEAN Override;
} RegistryModel;

VOID RegistryModelReset(VOID)
{
    RtlZeroMemory(&RegistryModel, sizeof(RegistryModel));
}

VOID RegistryModelValue(PCWSTR Name, ULONG Type, const VOID* Data, ULONG Length)
{
    RegistryModel.Name = Name;
    RegistryModel.Type = Type;
    RegistryModel.Data = Data;
    RegistryModel.Length = Length;
}

VOID RegistryModelQueryResult(NTSTATUS Status, ULONG ResultLength, ULONG DataLength)
{
    RegistryModel.Override = TRUE;
    RegistryModel.QueryStatus = Status;
    RegistryModel.ResultLength = ResultLength;
    RegistryModel.DataLength = DataLength;
}

VOID RegistryModelOpenStatus(NTSTATUS Status)
{
    RegistryModel.OpenStatus = Status;
}

NTSTATUS ZwOpenKey(PHANDLE Handle, ACCESS_MASK Access, POBJECT_ATTRIBUTES Attributes)
{
    UNREFERENCED_PARAMETER(Access);
    UNREFERENCED_PARAMETER(Attributes);
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "ZwOpenKey");
    *Handle = C_CAST(HANDLE, &RegistryModel);
    return RegistryModel.OpenStatus;
}

NTSTATUS ZwQueryValueKey(HANDLE Handle, PUNICODE_STRING Name,
    KEY_VALUE_INFORMATION_CLASS Class, PVOID Buffer, ULONG Length, PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Class);
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "ZwQueryValueKey");

    if (!RegistryModel.Name || wcslen(RegistryModel.Name) * sizeof(WCHAR) != Name->Length ||
        wmemcmp(RegistryModel.Name, Name->Buffer, Name->Length / sizeof(WCHAR)))
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const ULONG header = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
    *ResultLength = RegistryModel.Override ? RegistryModel.ResultLength : header + RegistryModel.Length;

    if (RegistryModel.Override && !NT_SUCCESS(RegistryModel.QueryStatus))
    {
        return RegistryModel.QueryStatus;
    }

    if (RegistryModel.Length > Length - header)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    PKEY_VALUE_PARTIAL_INFORMATION info = C_CAST(PKEY_VALUE_PARTIAL_INFORMATION, Buffer);
    info->TitleIndex = 0;
    info->Type = RegistryModel.Type;
    info->DataLength = RegistryModel.Override ? RegistryModel.DataLength : RegistryModel.Length;
    RtlCopyMemory(info->Data, RegistryModel.Data, RegistryModel.Length);
    return STATUS_SUCCESS;
}
