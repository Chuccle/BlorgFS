#pragma once

extern struct GLOBAL
{
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT FileSystemDeviceObject;
    PDEVICE_OBJECT DiskDeviceObject;
    PADDRINFOEXW   RemoteAddressInfo;
    PVOID LazyWriteThread;
    ULONG ProcessorCount;
} global;