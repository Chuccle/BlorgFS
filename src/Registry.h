#pragma once

//
// Optional Parameters overrides. Caller seeds global defaults and supplies
// counted output buffers. PASSIVE_LEVEL; missing/invalid values are ignored.
//
#define BLORGFS_REG_PORT_MAX_CHARS 8

VOID BlorgReadRegistryConfig(PUNICODE_STRING ServiceRegistryPath,
    PUNICODE_STRING PortOut, PUNICODE_STRING HostOut, PUNICODE_STRING DiskCachePathOut);
