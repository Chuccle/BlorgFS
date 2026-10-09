#pragma once

//
// Scripted Parameters values for the real Registry.c. The query returns
// the NT header/data layout, including controllable malformed lengths.
//
typedef enum _KEY_VALUE_INFORMATION_CLASS
{
    KeyValuePartialInformation = 2
} KEY_VALUE_INFORMATION_CLASS;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;

#ifndef STATUS_OBJECT_TYPE_MISMATCH
#define STATUS_OBJECT_TYPE_MISMATCH C_CAST(NTSTATUS, 0xC0000024L)
#endif

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH C_CAST(NTSTATUS, 0xC0000004L)
#endif

#ifdef __cplusplus
extern "C" {
#endif

VOID RegistryModelReset(VOID);
VOID RegistryModelValue(PCWSTR Name, ULONG Type, const VOID* Data, ULONG Length);
VOID RegistryModelQueryResult(NTSTATUS Status, ULONG ResultLength, ULONG DataLength);
VOID RegistryModelOpenStatus(NTSTATUS Status);
NTSTATUS ZwOpenKey(PHANDLE Handle, ACCESS_MASK Access, POBJECT_ATTRIBUTES Attributes);
NTSTATUS ZwQueryValueKey(HANDLE Handle, PUNICODE_STRING Name,
    KEY_VALUE_INFORMATION_CLASS Class, PVOID Buffer, ULONG Length, PULONG ResultLength);

#ifdef __cplusplus
}
#endif
