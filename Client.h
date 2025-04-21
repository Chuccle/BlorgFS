#pragma once

typedef struct _HTTP_FILE_BUFFER
{
    PCHAR BodyBuffer;
    SIZE_T BodyBufferSize;
    PCHAR BaseAddress;
} HTTP_FILE_BUFFER, * PHTTP_FILE_BUFFER;

NTSTATUS InitialiseHttpClient(void);
void CleanupHttpClient(void);

NTSTATUS GetHttpAddrInfo(const PUNICODE_STRING NodeName, const PUNICODE_STRING ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS GetHttpDirectoryInfo(const PUNICODE_STRING Path, PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo);
void FreeHttpDirectoryInfo(PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo);

NTSTATUS GetHttpFileInformation(const PUNICODE_STRING Path, PDIRECTORY_ENTRY DirectoryEntryInfo, PBOOLEAN isDir);

NTSTATUS GetHttpFile(const PUNICODE_STRING Path, SIZE_T StartOffset, SIZE_T Length, PHTTP_FILE_BUFFER OutputBuffer);
void FreeHttpFile(PHTTP_FILE_BUFFER FileBuffer);