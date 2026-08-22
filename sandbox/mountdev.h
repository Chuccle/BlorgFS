#pragma once

//
// The mount-manager IOCTL surface DevIoCtrl.c answers.
//
// A volume that does not respond to these does not get a drive letter, and
// the failure surfaces as "Incorrect function" from an unrelated operation
// long before any file IRP arrives -- so these codes are load-bearing even
// though the structures behind them are trivial.
//

#define MOUNTDEVCONTROLTYPE 0x0000004D

#define IOCTL_MOUNTDEV_QUERY_UNIQUE_ID \
    CTL_CODE(MOUNTDEVCONTROLTYPE, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_QUERY_DEVICE_NAME \
    CTL_CODE(MOUNTDEVCONTROLTYPE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME \
    CTL_CODE(MOUNTDEVCONTROLTYPE, 11, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MOUNTDEV_LINK_CREATED     CTL_CODE(MOUNTDEVCONTROLTYPE, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_LINK_DELETED     CTL_CODE(MOUNTDEVCONTROLTYPE, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _MOUNTDEV_UNIQUE_ID
{
    USHORT UniqueIdLength;
    UCHAR UniqueId[1];
} MOUNTDEV_UNIQUE_ID, * PMOUNTDEV_UNIQUE_ID;

typedef struct _MOUNTDEV_NAME
{
    USHORT NameLength;
    WCHAR Name[1];
} MOUNTDEV_NAME, * PMOUNTDEV_NAME;

typedef struct _MOUNTDEV_SUGGESTED_LINK_NAME
{
    BOOLEAN UseOnlyIfThereAreNoOtherLinks;
    USHORT NameLength;
    WCHAR Name[1];
} MOUNTDEV_SUGGESTED_LINK_NAME, * PMOUNTDEV_SUGGESTED_LINK_NAME;
