#pragma once

typedef struct _HTTP_FILE_BUFFER
{
    PCHAR BodyBuffer;
    SIZE_T BodyBufferSize;
    PCHAR BaseAddress;
} HTTP_FILE_BUFFER, * PHTTP_FILE_BUFFER;

NTSTATUS InitialiseHttpClient(void);
void CleanupHttpClient(void);

NTSTATUS GetHttpAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS GetHttpDirectoryInfo(const UNICODE_STRING* Path, PDIRECTORY_INFO* OutDirInfo);
void FreeHttpDirectoryInfo(PDIRECTORY_INFO DirInfo);

NTSTATUS GetHttpFileInformation(const UNICODE_STRING* Path, PDIRECTORY_ENTRY_METADATA DirectoryEntryInfo);

NTSTATUS GetHttpFile(const UNICODE_STRING* Path, SIZE_T StartOffset, SIZE_T Length, PHTTP_FILE_BUFFER OutputBuffer);
void FreeHttpFile(PHTTP_FILE_BUFFER FileBuffer);