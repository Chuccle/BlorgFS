#pragma once

NTSTATUS InitialiseHttpClient(void);
void CleanupHttpClient(void);

NTSTATUS GetHttpAddrInfo(PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS GetHttpDirectoryInfo(const PUNICODE_STRING Path, PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo);
void FreeHttpDirectoryInfo(PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo);

NTSTATUS FindHttpFile(const PUNICODE_STRING Path, PBOOLEAN Directory, PBOOLEAN Found);