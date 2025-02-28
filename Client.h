#pragma once

NTSTATUS InitialiseHttpClient(void);
void CleanupHttpClient(void);

NTSTATUS GetHttpAddrInfo(PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS GetHttpDirectoryInfo(const PANSI_STRING path);