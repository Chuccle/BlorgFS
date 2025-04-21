#include "Driver.h"
#include "Socket.h"

#define FLATCC_NO_ASSERT

#pragma warning(push)
#pragma warning(disable: 28110) // We don't use floating point numbers in our schema so no need to mess about with saving CPU state.
#include "metadata_flatbuffer_reader.h"
#include "metadata_flatbuffer_verifier.h"
#pragma warning(pop)

#include "picohttpparser.h"

#define N_DIGITS_INT_MAX_INTERNAL(N) \
    ((N) < 10 ? 1 : \
     (N) < 100 ? 2 : \
     (N) < 1000 ? 3 : \
     (N) < 10000 ? 4 : \
     (N) < 100000 ? 5 : \
     (N) < 1000000 ? 6 : \
     (N) < 10000000 ? 7 : \
     (N) < 100000000 ? 8 : \
     (N) < 1000000000 ? 9 : \
     (N) < 10000000000ULL ? 10 : \
     (N) < 100000000000ULL ? 11 : \
     (N) < 1000000000000ULL ? 12 : \
     (N) < 10000000000000ULL ? 13 : \
     (N) < 100000000000000ULL ? 14 : \
     (N) < 1000000000000000ULL ? 15 : \
     (N) < 10000000000000000ULL ? 16 : \
     (N) < 100000000000000000ULL ? 17 : \
     (N) < 1000000000000000000ULL ? 18 : \
     19) // For ULLONG_MAX, which is at least 2^64-1, requiring 20 digits.
          // This macro can be extended if needed, but 19 covers up to 10^19-1.

// consider intsafe here when relying on the content length. 
// process flatbuffers inside a system thread.
// we should probably move all of this to a user mode service with inverted call model.

typedef struct _HTTP_BUFFER_INFO
{
    const struct phr_header* Headers;
    SIZE_T HeaderCount;
    PCHAR BodyBuffer;
    SIZE_T BodyBufferSize;
} HTTP_BUFFER_INFO, * PHTTP_BUFFER_INFO;

static NTSTATUS StrToSize(const char* AsciiBuffer, SIZE_T Length, PSIZE_T Result)
{
    if (!AsciiBuffer || !Result || 0 == Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *Result = 0;
    SIZE_T i = 0;

    // Process digits
    while (i < Length && AsciiBuffer[i] >= '0' && AsciiBuffer[i] <= '9')
    {
        // Check for overflow
        if (*Result > (SIZE_MAX / 10) || (*Result == (SIZE_MAX / 10) && (AsciiBuffer[i] - '0') > (SIZE_MAX % 10)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        *Result = *Result * 10 + (AsciiBuffer[i] - '0');
        i++;
    }

    if (i < Length)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GetContentLengthFromHeaders(const struct phr_header* Headers, SIZE_T HeaderCount, PSIZE_T ContentLength)
{
    ANSI_STRING contentLengthBuffer = RTL_CONSTANT_STRING("content-length");

    for (SIZE_T i = 0; i < HeaderCount; ++i)
    {
        // Find Content length key
        if (Headers[i].name_len == 14)
        {
            ANSI_STRING headerNameBuffer =
            {
                .Buffer = (PCHAR)Headers[i].name,
                .MaximumLength = (USHORT)Headers[i].name_len,
                .Length = (USHORT)Headers[i].name_len
            };

            if (!RtlEqualString(&headerNameBuffer, &contentLengthBuffer, TRUE))
            {
                continue;
            }

            // Convert into numerical value
            return StrToSize(Headers[i].value, Headers[i].value_len, ContentLength);
        }
    }

    return STATUS_NOT_FOUND;
}

static NTSTATUS AlignBuffer(char* OriginalBuffer, SIZE_T BufferSize, SIZE_T ContentLength, PCHAR* AlignedBuffer, PBOOLEAN Allocated)
{
    *Allocated = FALSE;

    int alignmentOffset = (0x8 - ((UINT_PTR)OriginalBuffer & 0x7)) & 0x7;

    // Buffer is already aligned
    if (0 == alignmentOffset)
    {
        *AlignedBuffer = OriginalBuffer;
        return STATUS_SUCCESS;
    }

    // See if we can reuse some of the slack in the original buffer
    if (((ULONG64)alignmentOffset + ContentLength) <= BufferSize)
    {
        RtlMoveMemory(OriginalBuffer + alignmentOffset, OriginalBuffer, ContentLength);
        *AlignedBuffer = OriginalBuffer + alignmentOffset;
        return STATUS_SUCCESS;
    }

    // Need to allocate a new aligned buffer
    *Allocated = TRUE;
    *AlignedBuffer = ExAllocatePoolUninitialized(PagedPool, ContentLength, 'test');

    if (!*AlignedBuffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(*AlignedBuffer, OriginalBuffer, ContentLength);
    return STATUS_SUCCESS;
}

static NTSTATUS ProcessEntries(BlorgMetaFlat_DirectoryEntriesMetadata_table_t entriesMetadata, uint64_t entryCount, PDIRECTORY_INFO outDirInfo)
{
    if (!entriesMetadata || !outDirInfo)
    {
        BLORGFS_PRINT("ProcessEntries() - invalid arguments\n");
        return STATUS_INVALID_PARAMETER;
    }

    PDIRECTORY_ENTRY entries = ExAllocatePoolZero(PagedPool, entryCount * sizeof(DIRECTORY_ENTRY), 'DBLR');

    if (!entries)
    {
        BLORGFS_PRINT("ProcessEntries() - failed entries alloc\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    flatbuffers_string_vec_t name_vector = BlorgMetaFlat_DirectoryEntriesMetadata_name(entriesMetadata);
    flatbuffers_uint64_vec_t size_vector = BlorgMetaFlat_DirectoryEntriesMetadata_size(entriesMetadata);
    flatbuffers_uint64_vec_t last_created_vector = BlorgMetaFlat_DirectoryEntriesMetadata_created(entriesMetadata);
    flatbuffers_uint64_vec_t last_accessed_vector = BlorgMetaFlat_DirectoryEntriesMetadata_accessed(entriesMetadata);
    flatbuffers_uint64_vec_t last_modified_vector = BlorgMetaFlat_DirectoryEntriesMetadata_modified(entriesMetadata);

    if (entryCount != flatbuffers_string_vec_len(name_vector) ||
        entryCount != flatbuffers_uint64_vec_len(size_vector) ||
        entryCount != flatbuffers_uint64_vec_len(last_created_vector) ||
        entryCount != flatbuffers_uint64_vec_len(last_accessed_vector) ||
        entryCount != flatbuffers_uint64_vec_len(last_modified_vector))
    {
        BLORGFS_PRINT("ProcessEntries() - vector length to entry count mismatch\n");
        ExFreePool(entries);
        return STATUS_INVALID_PARAMETER;
    }

    for (size_t i = 0; i < entryCount; ++i)
    {
        flatbuffers_string_t name = flatbuffers_string_vec_at(name_vector, i);
        uint64_t size = flatbuffers_uint64_vec_at(size_vector, i);
        uint64_t created = flatbuffers_uint64_vec_at(last_created_vector, i);
        uint64_t accessed = flatbuffers_uint64_vec_at(last_accessed_vector, i);
        uint64_t modified = flatbuffers_uint64_vec_at(last_modified_vector, i);

        if (0 == flatbuffers_string_len(name))
        {
            BLORGFS_PRINT("ProcessEntries() - string entry length is 0\n");
            for (size_t j = 0; j < i; j++)
            {
                RtlFreeUnicodeString(&(entries[j].Name));
            }
            ExFreePool(entries);
            return STATUS_INVALID_PARAMETER;
        }

        // Convert char* to UNICODE_STRING
        UTF8_STRING utf8name;
        RtlInitUTF8String(&utf8name, name);

        NTSTATUS status = RtlUTF8StringToUnicodeString(&(entries[i].Name), &utf8name, TRUE);
        if (!NT_SUCCESS(status))
        {
            BLORGFS_PRINT("ProcessEntries() - failed RtlAnsiStringToUnicodeString\n");
            for (size_t j = 0; j < i; j++)
            {
                RtlFreeUnicodeString(&(entries[j].Name));
            }
            ExFreePool(entries);
            return status;
        }

        entries[i].Size = size;
        entries[i].CreationTime = created;
        entries[i].LastAccessedTime = accessed;
        entries[i].LastModifiedTime = modified;

        BLORGFS_PRINT("name=%s -- size=%llu -- created=%llu -- accessed=%llu -- modified=%llu\n", name, size, created, accessed, modified);
    }

    outDirInfo->Entries = entries;
    outDirInfo->EntryCount = entryCount;

    return STATUS_SUCCESS;
}

static NTSTATUS DeserializeDirectoryInfoFlatBuffer(const PHTTP_BUFFER_INFO HttpInfo, PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo)
{
    if (!HttpInfo->Headers || !HttpInfo->BodyBuffer || 0 == HttpInfo->BodyBufferSize)
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - params are weird\n");
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T contentLength;

    NTSTATUS result = GetContentLengthFromHeaders(HttpInfo->Headers, HttpInfo->HeaderCount, &contentLength);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - Failed to parse headers to get content length\n");
        return result;
    }

    if (HttpInfo->BodyBufferSize < contentLength)
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - buffer to small for needed payload\n");
        return STATUS_INVALID_PARAMETER;
    }

    PCHAR alignedBuffer;
    BOOLEAN bufferAllocated;

    result = AlignBuffer(HttpInfo->BodyBuffer, HttpInfo->BodyBufferSize, contentLength, &alignedBuffer, &bufferAllocated);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - buffer alignment failure\n");
        return result;
    }

    int verifyCode = BlorgMetaFlat_Directory_verify_as_root(alignedBuffer, contentLength);

    if (flatcc_verify_ok != verifyCode)
    {
        if (bufferAllocated)
        {
            ExFreePool(alignedBuffer);
        }

        BLORGFS_PRINT("DeserializeFlatBuffer() - %s\n", flatcc_verify_error_string(verifyCode));

        return STATUS_INVALID_PARAMETER;
    }

    BlorgMetaFlat_Directory_table_t directory = BlorgMetaFlat_Directory_as_root(alignedBuffer);

    if (!directory)
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - directory knackered\n");
        if (bufferAllocated)
        {
            ExFreePool(alignedBuffer);
        }
        return STATUS_INVALID_PARAMETER;
    }

    uint64_t fileCount = BlorgMetaFlat_Directory_file_count(directory);
    uint64_t directoryCount = BlorgMetaFlat_Directory_directory_count(directory);

    if (0 == (fileCount + directoryCount))
    {
        if (bufferAllocated)
        {
            ExFreePool(alignedBuffer);
        }
        // empty dir can be considered valid
        return STATUS_SUCCESS;
    }

    if (0 < fileCount)
    {
        BlorgMetaFlat_DirectoryEntriesMetadata_table_t files = BlorgMetaFlat_Directory_files(directory);

        if (!files)
        {
            BLORGFS_PRINT("DeserializeFlatBuffer() - files vector suggested but not valid\n");
            if (bufferAllocated)
            {
                ExFreePool(alignedBuffer);
            }
            return STATUS_INVALID_PARAMETER;
        }

        result = ProcessEntries(files, fileCount, FileDirInfo);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("DeserializeFlatBuffer() - error processing files\n");
            if (bufferAllocated)
            {
                ExFreePool(alignedBuffer);
            }
            return result;
        }
    }

    if (0 < directoryCount)
    {
        BlorgMetaFlat_DirectoryEntriesMetadata_table_t subDirs = BlorgMetaFlat_Directory_directories(directory);

        if (!subDirs)
        {
            BLORGFS_PRINT("DeserializeFlatBuffer() - directories vector suggested but not valid\n");
            FreeHttpDirectoryInfo(NULL, FileDirInfo);
            if (bufferAllocated)
            {
                ExFreePool(alignedBuffer);
            }
            return STATUS_INVALID_PARAMETER;
        }

        result = ProcessEntries(subDirs, directoryCount, SubDirInfo);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("DeserializeFlatBuffer() - error processing directories\n");
            FreeHttpDirectoryInfo(NULL, FileDirInfo);
            if (bufferAllocated)
            {
                ExFreePool(alignedBuffer);
            }
            return result;
        }
    }

    if (bufferAllocated)
    {
        ExFreePool(alignedBuffer);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS DeserializeDirectoryEntryInfoFlatBuffer(const PHTTP_BUFFER_INFO HttpInfo, PDIRECTORY_ENTRY DirEntryInfo, PBOOLEAN IsDir)
{
    if (!HttpInfo->Headers || !HttpInfo->BodyBuffer || 0 == HttpInfo->BodyBufferSize)
    {
        BLORGFS_PRINT("DeserializeDirectoryEntryInfoFlatBuffer() - params are weird\n");
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T contentLength;

    NTSTATUS result = GetContentLengthFromHeaders(HttpInfo->Headers, HttpInfo->HeaderCount, &contentLength);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeDirectoryEntryInfoFlatBuffer() - Failed to parse headers to get content length\n");
        return result;
    }

    if (HttpInfo->BodyBufferSize < contentLength)
    {
        BLORGFS_PRINT("DeserializeDirectoryEntryInfoFlatBuffer() - buffer to small for needed payload\n");
        return STATUS_INVALID_PARAMETER;
    }

    PCHAR alignedBuffer;
    BOOLEAN bufferAllocated;

    result = AlignBuffer(HttpInfo->BodyBuffer, HttpInfo->BodyBufferSize, contentLength, &alignedBuffer, &bufferAllocated);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeDirectoryEntryInfoFlatBuffer() - buffer alignment failure\n");
        return result;
    }

    int verifyCode = BlorgMetaFlat_DirectoryEntryMetadata_verify_as_root(alignedBuffer, contentLength);

    if (flatcc_verify_ok != verifyCode)
    {
        if (bufferAllocated)
        {
            ExFreePool(alignedBuffer);
        }

        BLORGFS_PRINT("DeserializeDirectoryEntryInfoFlatBuffer() - %s\n", flatcc_verify_error_string(verifyCode));

        return STATUS_INVALID_PARAMETER;
    }

    BlorgMetaFlat_DirectoryEntryMetadata_table_t dirEntMeta = BlorgMetaFlat_DirectoryEntryMetadata_as_root(alignedBuffer);

    if (!dirEntMeta)
    {
        if (bufferAllocated)
        {
            ExFreePool(alignedBuffer);
        }

        return STATUS_INVALID_PARAMETER;
    }

    DirEntryInfo->Size = BlorgMetaFlat_DirectoryEntryMetadata_size(dirEntMeta);
    DirEntryInfo->CreationTime = BlorgMetaFlat_DirectoryEntryMetadata_created(dirEntMeta);
    DirEntryInfo->LastAccessedTime = BlorgMetaFlat_DirectoryEntryMetadata_accessed(dirEntMeta);
    DirEntryInfo->LastModifiedTime = BlorgMetaFlat_DirectoryEntryMetadata_modified(dirEntMeta);

    *IsDir = BlorgMetaFlat_DirectoryEntryMetadata_directory(dirEntMeta);

    if (bufferAllocated)
    {
        ExFreePool(alignedBuffer);
    }

    return STATUS_SUCCESS;
}

#define HEX_TO_CHAR(x) ((x) < 10 ? '0' + (x) : 'A' + (x) - 10)

static BOOLEAN IsCharacterSafeForUrl(UCHAR c)
{
    // RFC 3986 unreserved characters: ALPHA / DIGIT / "-" / "." / "_" / "~"

    if ((c >= 'A' && c <= 'Z') ||    // Uppercase letters
        (c >= 'a' && c <= 'z') ||    // Lowercase letters  
        (c >= '0' && c <= '9') ||    // Digits
        c == '-' ||                   // Hyphen
        c == '.' ||                   // Period
        c == '_' ||                   // Underscore
        c == '~')                     // Tilde
    {
        return TRUE;
    }

    return FALSE;
}

static NTSTATUS UrlEncodeUnicodeString(PUNICODE_STRING InputString, PUNICODE_STRING OutputString, BOOLEAN AllocateDestination)
{
    UTF8_STRING utf8String;

    NTSTATUS status = RtlUnicodeStringToUTF8String(&utf8String, InputString, TRUE);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PUCHAR utf8Buffer = (PUCHAR)utf8String.Buffer;
    ULONG utf8Length = utf8String.Length;
    SIZE_T estimatedLength = 0;

    // First pass: calculate the estimated length of the encoded string
    for (ULONG i = 0; i < utf8Length; i++)
    {
        UCHAR c = utf8Buffer[i];

        if (IsCharacterSafeForUrl(c))
        {
            estimatedLength++;
        }
        else
        {
            estimatedLength += 3; // Percent-encoded characters need 3 chars (%XX)
        }
    }

    if (AllocateDestination)
    {
        OutputString->Buffer = (PWCHAR)ExAllocatePoolUninitialized(
            PagedPool,
            estimatedLength * sizeof(WCHAR),
            'URLE'
        );

        if (NULL == OutputString->Buffer)
        {
            RtlFreeUTF8String(&utf8String);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        OutputString->MaximumLength = (USHORT)(estimatedLength * sizeof(WCHAR));
    }
    else if (OutputString->MaximumLength < estimatedLength * sizeof(WCHAR))
    {
        RtlFreeUTF8String(&utf8String);
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Second pass: perform actual URL encoding
    ULONG j = 0;

    for (ULONG i = 0; i < utf8Length; i++)
    {
        UCHAR c = utf8Buffer[i];

        if (IsCharacterSafeForUrl(c))
        {
            if (j + 1 > OutputString->MaximumLength / sizeof(WCHAR))
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            OutputString->Buffer[j++] = (WCHAR)c;
        }
        else
        {
            if (j + 3 > OutputString->MaximumLength / sizeof(WCHAR))
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            OutputString->Buffer[j++] = L'%';
            OutputString->Buffer[j++] = (WCHAR)HEX_TO_CHAR((c >> 4) & 0xF);
            OutputString->Buffer[j++] = (WCHAR)HEX_TO_CHAR(c & 0xF);
        }
    }

    OutputString->Length = (USHORT)(j * sizeof(WCHAR));

    RtlFreeUTF8String(&utf8String);

    return status;
}

NTSTATUS GetHttpDirectoryInfo(const PUNICODE_STRING Path, PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo)
{
    if (!Path || 0 == Path->Length || !Path->Buffer)
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo - Invalid path parameter\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (!SubDirInfo || !FileDirInfo)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SubDirInfo->Entries = NULL;
    SubDirInfo->EntryCount = 0;

    FileDirInfo->Entries = NULL;
    FileDirInfo->EntryCount = 0;

    UNICODE_STRING encodedPath;
    NTSTATUS result = UrlEncodeUnicodeString(Path, &encodedPath, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo() - No encoded buffer\n");
        return result;
    }

    char sendBufferFormat[] =
        "GET /get_dir_info?path=%wZ HTTP/1.1\r\n"
        "Host: blorgfs-server.blorg.lan\r\n"
        "Connection: close\r\n"
        "\r\n";

    ULONG sendBufferSize = sizeof(sendBufferFormat) + encodedPath.Length;
    char* sendBuffer = ExAllocatePoolZero(PagedPool, sendBufferSize, 'BOOB');

    if (!sendBuffer)
    {
        ExFreePool(encodedPath.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    result = RtlStringCbPrintfA(sendBuffer, sendBufferSize, sendBufferFormat, &encodedPath);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo() - No send buffer alloc\n");
        ExFreePool(sendBuffer);
        ExFreePool(encodedPath.Buffer);
        return result;
    }

    BLORGFS_PRINT("GetHttpDirectoryInfo() - sending [ %s ]\n", sendBuffer);

    ExFreePool(encodedPath.Buffer);

    ULONG receiveBufferSize = 0x4000;
    char* receiveBuffer = ExAllocatePoolUninitialized(PagedPool, receiveBufferSize, 'TEST');

    if (!receiveBuffer)
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo() - No receive buffer alloc\n");
        ExFreePool(sendBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PKSOCKET socket;
    result = CreateWskSocket(&socket, SOCK_STREAM, IPPROTO_TCP, 0, global.RemoteAddressInfo->ai_addr);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to create socket\n");
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = SendRecvWsk(socket, sendBuffer, (ULONG)strlen(sendBuffer), NULL, 0, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to send\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    ULONG totalBytesWritten = 0;
    ULONG bytesWritten = 0;

    do
    {
        if (totalBytesWritten >= receiveBufferSize)
        {
            int minor_version;
            int status;
            const char* msg;
            SIZE_T msg_len;
            struct phr_header headers[4];
            SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

            int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

            if (bytesProcessed < 0)
            {
                BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to parse HTTP response\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INVALID_PARAMETER;
            }

            SIZE_T contentLength;
            result = GetContentLengthFromHeaders(headers, num_headers, &contentLength);

            if (!NT_SUCCESS(result))
            {
                BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to parse content length header\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return result;
            }

            PCHAR newReceiveBuffer = ReallocateBufferUninitialized(receiveBuffer, receiveBufferSize, PagedPool, (bytesProcessed + contentLength), 'BHDI');

            if (newReceiveBuffer == receiveBuffer)
            {
                BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to allocate new receive buffer\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            receiveBufferSize = bytesProcessed + (ULONG)contentLength;
            receiveBuffer = newReceiveBuffer;
        }

        result = SendRecvWsk(socket, receiveBuffer + totalBytesWritten, receiveBufferSize - totalBytesWritten, &bytesWritten, 0, FALSE);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("GetHttpDirectoryInfo() - Failed to receive\n");
            CloseWskSocket(socket);
            ExFreePool(receiveBuffer);
            ExFreePool(sendBuffer);
            return result;
        }

        totalBytesWritten += bytesWritten;
    }
    while (0 < bytesWritten);

    int minor_version;
    int status;
    const char* msg;
    SIZE_T msg_len;
    struct phr_header headers[4];
    SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

    int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

    if (0 > bytesProcessed || 200 != status)
    {
        BLORGFS_PRINT("phr_parse_response() - Failed to parse\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    HTTP_BUFFER_INFO httpInfo =
    {
        .BodyBuffer = (receiveBuffer + bytesProcessed),
        .BodyBufferSize = ((SIZE_T)receiveBufferSize - bytesProcessed),
        .Headers = headers,
        .HeaderCount = num_headers
    };

    result = DeserializeDirectoryInfoFlatBuffer(&httpInfo, SubDirInfo, FileDirInfo);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - Failed to deserialise\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = CloseWskSocket(socket);
    ExFreePool(receiveBuffer);
    ExFreePool(sendBuffer);

    return result;
}

void FreeHttpDirectoryInfo(PDIRECTORY_INFO SubDirInfo, PDIRECTORY_INFO FileDirInfo)
{
    if (SubDirInfo && SubDirInfo->Entries)
    {
        for (SIZE_T i = 0; i < SubDirInfo->EntryCount; i++)
        {
            RtlFreeUnicodeString(&(SubDirInfo->Entries[i].Name));
        }
        ExFreePool(SubDirInfo->Entries);

        RtlZeroMemory(SubDirInfo, sizeof(DIRECTORY_INFO));
    }

    if (FileDirInfo && FileDirInfo->Entries)
    {
        for (SIZE_T i = 0; i < FileDirInfo->EntryCount; i++)
        {
            RtlFreeUnicodeString(&(FileDirInfo->Entries[i].Name));
        }
        ExFreePool(FileDirInfo->Entries);

        RtlZeroMemory(FileDirInfo, sizeof(DIRECTORY_INFO));
    }
}

NTSTATUS GetHttpFileInformation(const PUNICODE_STRING Path, PDIRECTORY_ENTRY DirectoryEntryInfo, PBOOLEAN isDir)
{
    if (!Path || 0 == Path->Length || !Path->Buffer)
    {
        BLORGFS_PRINT("GetHttpFileInformation() - Invalid path parameter\n");
        return STATUS_INVALID_PARAMETER;
    }

    UNICODE_STRING encodedPath;
    NTSTATUS result = UrlEncodeUnicodeString(Path, &encodedPath, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFileInformation() - No encoded buffer\n");
        return result;
    }

    char sendBufferFormat[] =
        "GET /get_dir_entry_info?path=%wZ HTTP/1.1\r\n"
        "Host: blorgfs-server.blorg.lan\r\n"
        "Connection: close\r\n"
        "\r\n";

    ULONG sendBufferSize = sizeof(sendBufferFormat) + encodedPath.Length;
    char* sendBuffer = ExAllocatePoolZero(PagedPool, sendBufferSize, 'BOOB');

    if (!sendBuffer)
    {
        ExFreePool(encodedPath.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    result = RtlStringCbPrintfA(sendBuffer, sendBufferSize, sendBufferFormat, &encodedPath);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFileInformation() - No send buffer alloc\n");
        ExFreePool(sendBuffer);
        ExFreePool(encodedPath.Buffer);
        return result;
    }

    ExFreePool(encodedPath.Buffer);

    ULONG receiveBufferSize = PAGE_SIZE;
    char* receiveBuffer = ExAllocatePoolUninitialized(PagedPool, receiveBufferSize, 'TEST');

    if (!receiveBuffer)
    {
        BLORGFS_PRINT("GetHttpFileInformation() - No receive buffer alloc\n");
        ExFreePool(sendBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PKSOCKET socket;
    result = CreateWskSocket(&socket, SOCK_STREAM, IPPROTO_TCP, 0, global.RemoteAddressInfo->ai_addr);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFileInformation() - Failed to create socket\n");
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = SendRecvWsk(socket, sendBuffer, (ULONG)strlen(sendBuffer), NULL, 0, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFileInformation() - Failed to send\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    ULONG totalBytesWritten = 0;
    ULONG bytesWritten = 0;

    do
    {
        if (totalBytesWritten >= receiveBufferSize)
        {
            int minor_version;
            int status;
            const char* msg;
            SIZE_T msg_len;
            struct phr_header headers[4];
            SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

            int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

            if (bytesProcessed < 0)
            {
                BLORGFS_PRINT("GetHttpFileInformation() - Failed to parse HTTP response\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INVALID_PARAMETER;
            }

            SIZE_T contentLength;
            result = GetContentLengthFromHeaders(headers, num_headers, &contentLength);

            if (!NT_SUCCESS(result))
            {
                BLORGFS_PRINT("GetHttpFileInformation() - Failed to parse content length header\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return result;
            }

            PCHAR newReceiveBuffer = ReallocateBufferUninitialized(receiveBuffer, receiveBufferSize, PagedPool, (bytesProcessed + contentLength), 'BHDI');

            if (newReceiveBuffer == receiveBuffer)
            {
                BLORGFS_PRINT("GetHttpFileInformation() - Failed to allocate new receive buffer\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            receiveBufferSize = bytesProcessed + (ULONG)contentLength;
            receiveBuffer = newReceiveBuffer;
        }

        result = SendRecvWsk(socket, receiveBuffer + totalBytesWritten, receiveBufferSize - totalBytesWritten, &bytesWritten, 0, FALSE);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("GetHttpFileInformation() - Failed to receive\n");
            CloseWskSocket(socket);
            ExFreePool(receiveBuffer);
            ExFreePool(sendBuffer);
            return result;
        }

        totalBytesWritten += bytesWritten;
    }
    while (0 < bytesWritten);

    int minor_version;
    int status;
    const char* msg;
    SIZE_T msg_len;
    struct phr_header headers[4];
    SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

    int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

    if (0 > bytesProcessed || 200 != status)
    {
        ASSERT(0 <= bytesProcessed);

        BLORGFS_PRINT("phr_parse_response() - Failed to parse\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);

        return (404 == status) ? STATUS_NOT_FOUND : STATUS_INVALID_PARAMETER;
    }

    HTTP_BUFFER_INFO httpInfo =
    {
        .BodyBuffer = (receiveBuffer + bytesProcessed),
        .BodyBufferSize = ((SIZE_T)receiveBufferSize - bytesProcessed),
        .Headers = headers,
        .HeaderCount = num_headers
    };

    result = DeserializeDirectoryEntryInfoFlatBuffer(&httpInfo, DirectoryEntryInfo, isDir);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("DeserializeFlatBuffer() - Failed to deserialise\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = CloseWskSocket(socket);
    ExFreePool(receiveBuffer);
    ExFreePool(sendBuffer);

    return result;
}

NTSTATUS GetHttpFile(const PUNICODE_STRING Path, SIZE_T StartOffset, SIZE_T Length, PHTTP_FILE_BUFFER OutputBuffer)
{
    if (!Path || 0 == Path->Length || !Path->Buffer)
    {
        BLORGFS_PRINT("GetHttpFile() - Invalid path parameter\n");
        return STATUS_INVALID_PARAMETER;
    }

    NT_ASSERT(0 != Length);

    UNICODE_STRING encodedPath;
    NTSTATUS result = UrlEncodeUnicodeString(Path, &encodedPath, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFile() - No encoded buffer\n");
        return result;
    }

    char sendBufferFormat[] =
        "GET /get_file?path=%wZ HTTP/1.1\r\n"
        "Host: blorgfs-server.blorg.lan\r\n"
        "Connection: close\r\n"
        "Range: bytes=%zu-%zu\r\n"
        "\r\n";

    const int ULLONG_MAX_DIGITS = 20;

    ULONG sendBufferSize = sizeof(sendBufferFormat) + encodedPath.Length + (ULLONG_MAX_DIGITS * 2);
    char* sendBuffer = ExAllocatePoolZero(PagedPool, sendBufferSize, 'BOOB');

    if (!sendBuffer)
    {
        ExFreePool(encodedPath.Buffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    result = RtlStringCbPrintfA(sendBuffer, sendBufferSize, sendBufferFormat, &encodedPath, StartOffset, (StartOffset + Length) - 1);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFile() - No send buffer alloc\n");
        ExFreePool(sendBuffer);
        ExFreePool(encodedPath.Buffer);
        return result;
    }

    ExFreePool(encodedPath.Buffer);

    ULONG receiveBufferSize = PAGE_SIZE;
    char* receiveBuffer = ExAllocatePoolUninitialized(PagedPool, receiveBufferSize, 'TEST');

    if (!receiveBuffer)
    {
        BLORGFS_PRINT("GetHttpFile() - No receive buffer alloc\n");
        ExFreePool(sendBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PKSOCKET socket;
    result = CreateWskSocket(&socket, SOCK_STREAM, IPPROTO_TCP, 0, global.RemoteAddressInfo->ai_addr);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("FindHttpFile() - Failed to create socket\n");
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = SendRecvWsk(socket, sendBuffer, (ULONG)strlen(sendBuffer), NULL, 0, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("GetHttpFile() - Failed to send\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    ULONG totalBytesWritten = 0;
    ULONG bytesWritten = 0;
    SIZE_T contentLength = 0;

    do
    {
        if (totalBytesWritten >= receiveBufferSize)
        {
            int minor_version;
            int status;
            const char* msg;
            SIZE_T msg_len;
            struct phr_header headers[7];
            SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

            int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

            if (bytesProcessed < 0)
            {
                BLORGFS_PRINT("GetHttpFile() - Failed to parse HTTP response\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INVALID_PARAMETER;
            }

            result = GetContentLengthFromHeaders(headers, num_headers, &contentLength);

            if (!NT_SUCCESS(result))
            {
                BLORGFS_PRINT("GetHttpFile() - Failed to parse content length header\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return result;
            }

            PCHAR newReceiveBuffer = ReallocateBufferUninitialized(receiveBuffer, receiveBufferSize, PagedPool, (bytesProcessed + contentLength), 'BHDI');

            if (newReceiveBuffer == receiveBuffer)
            {
                BLORGFS_PRINT("GetHttpFile() - Failed to allocate new receive buffer\n");
                CloseWskSocket(socket);
                ExFreePool(receiveBuffer);
                ExFreePool(sendBuffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            receiveBufferSize = bytesProcessed + (ULONG)contentLength;
            receiveBuffer = newReceiveBuffer;
        }

        result = SendRecvWsk(socket, receiveBuffer + totalBytesWritten, receiveBufferSize - totalBytesWritten, &bytesWritten, 0, FALSE);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("GetHttpFile() - Failed to receive\n");
            CloseWskSocket(socket);
            ExFreePool(receiveBuffer);
            ExFreePool(sendBuffer);
            return result;
        }

        totalBytesWritten += bytesWritten;

    }
    while (0 < bytesWritten);

    int minor_version;
    int status;
    const char* msg;
    SIZE_T msg_len;
    struct phr_header headers[7];
    SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);

    int bytesProcessed = phr_parse_response(receiveBuffer, totalBytesWritten, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

    if (0 > bytesProcessed || 206 != status)
    {
        BLORGFS_PRINT("phr_parse_response() - Failed to parse\n");
        CloseWskSocket(socket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    result = CloseWskSocket(socket);
    ExFreePool(sendBuffer);

    OutputBuffer->BodyBuffer = (receiveBuffer + bytesProcessed);
    OutputBuffer->BodyBufferSize = contentLength;
    OutputBuffer->BaseAddress = receiveBuffer;

    return result;
}

void FreeHttpFile(PHTTP_FILE_BUFFER FileBuffer)
{
    ExFreePool(FileBuffer->BaseAddress);
}

NTSTATUS GetHttpAddrInfo(PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo)
{
    return GetWskAddrInfo(NodeName, ServiceName, Hints, RemoteAddrInfo);
}

void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo)
{
    FreeWskAddrInfo(AddrInfo);
}

NTSTATUS InitialiseHttpClient(void)
{
    return InitialiseWskClient();
}

void CleanupHttpClient(void)
{
    CleanupWskClient();
}
