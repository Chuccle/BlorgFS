#include "Driver.h"
#include "Socket.h"

#define FLATCC_NO_ASSERT

#pragma warning(push)
#pragma warning(disable: 28110) // We don't use floating point numbers in our schema so no need to mess about with saving CPU state.
#include "metadata_flatbuffer_reader.h"
#include "metadata_flatbuffer_verifier.h"
#pragma warning(pop)

#include "picohttpparser.h"

//
// Convert a string to a size with bounds checking
//
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

static NTSTATUS DeserializeFlatBuffer(const struct phr_header* headers, SIZE_T headerCount, char* bodyBuffer, SIZE_T bodyBufferSize)
{

    if (0 == bodyBufferSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T contentLength = 0;

    KdBreakPoint();

    for (SIZE_T i = 0; i < headerCount; ++i)
    {
        // Find Content length key
        if (headers[i].name_len == 14)
        {
            ANSI_STRING contentLengthBuffer = RTL_CONSTANT_STRING("content-length");
            ANSI_STRING headerNameBuffer =
            {
                .Buffer = (PCHAR)headers[i].name,
                .MaximumLength = (USHORT)headers[i].name_len,
                .Length = (USHORT)headers[i].name_len
            };

            if (!RtlEqualString(&headerNameBuffer, &contentLengthBuffer, TRUE))
            {
                continue;
            }

            // Convert into numerical value
            NTSTATUS result = StrToSize(headers[i].value, headers[i].value_len, &contentLength);

            if (!NT_SUCCESS(result))
            {
                return result;
            }

            break;
        }
    }

    if (bodyBufferSize < contentLength)
    {
        return STATUS_INVALID_PARAMETER;
    }

    int alignmentOffset = (8 - ((UINT_PTR)bodyBuffer & 0x7)) & 0x7;
    PCHAR buffer;
    BOOLEAN allocated = FALSE;

    // Buffer is not aligned to 8 byte boundary
    if (0 < alignmentOffset)
    {
        // See if we can reuse some of the slack
        if (((ULONG64)alignmentOffset + contentLength) <= bodyBufferSize)
        {
            RtlMoveMemory(bodyBuffer + alignmentOffset, bodyBuffer, contentLength);
            buffer = bodyBuffer + alignmentOffset;
        }
        else
        {
            allocated = TRUE;

            buffer = ExAllocatePoolUninitialized(PagedPool, contentLength, 'test');

            if (!buffer)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlCopyMemory(buffer, bodyBuffer, contentLength);
        }
    }
    else
    {
        buffer = bodyBuffer;
    }

    int verifyCode = BlorgMetaFlat_Directory_verify_as_root(buffer, contentLength);

    if (flatcc_verify_ok != verifyCode)
    {
        KdBreakPoint();
        if (allocated)
        {
            ExFreePool(buffer);
        }
        return STATUS_INVALID_PARAMETER;

    }

    BlorgMetaFlat_Directory_table_t directory = BlorgMetaFlat_Directory_as_root(buffer);

    if (!directory)
    {
        KdBreakPoint();
        BLORGFS_PRINT("FLAT -- directory knackered\n");
        if (allocated)
        {
            ExFreePool(buffer);
        }
        return STATUS_INVALID_PARAMETER;
    }

    uint64_t fileCount = BlorgMetaFlat_Directory_file_count(directory);
    uint64_t directoryCount = BlorgMetaFlat_Directory_directory_count(directory);

    if (fileCount > 0)
    {
        BlorgMetaFlat_DirectoryEntriesMetadata_table_t files = BlorgMetaFlat_Directory_files(directory);

        if (files)
        {
            flatbuffers_string_vec_t name_vector = BlorgMetaFlat_DirectoryEntriesMetadata_name(files);
            flatbuffers_uint64_vec_t size_vector = BlorgMetaFlat_DirectoryEntriesMetadata_size(files);
            flatbuffers_uint64_vec_t last_accessed_vector = BlorgMetaFlat_DirectoryEntriesMetadata_accessed(files);
            flatbuffers_uint64_vec_t last_modified_vector = BlorgMetaFlat_DirectoryEntriesMetadata_modified(files);

            for (size_t i = 0; i < fileCount; ++i)
            {
                flatbuffers_string_t name = flatbuffers_string_vec_at(name_vector, i);
                uint64_t size = flatbuffers_uint64_vec_at(size_vector, i);
                uint64_t accessed = flatbuffers_uint64_vec_at(last_accessed_vector, i);
                uint64_t modified = flatbuffers_uint64_vec_at(last_modified_vector, i);

                (void)name;
                (void)size;
                (void)accessed;
                (void)modified;

                BLORGFS_PRINT("Querying file:%llu -- name=%s -- size=%llu -- accessed=%llu -- modified=%llu\n", i, name, size, accessed, modified);
            }

            KdBreakPoint();
            BLORGFS_PRINT("files worked!\n");

            //todo

        }
    }

    if (directoryCount > 0)
    {
        BlorgMetaFlat_DirectoryEntriesMetadata_table_t subDirs = BlorgMetaFlat_Directory_directories(directory);

        if (subDirs)
        {

            flatbuffers_string_vec_t name_vector = BlorgMetaFlat_DirectoryEntriesMetadata_name(subDirs);
            flatbuffers_uint64_vec_t size_vector = BlorgMetaFlat_DirectoryEntriesMetadata_size(subDirs);
            flatbuffers_uint64_vec_t last_accessed_vector = BlorgMetaFlat_DirectoryEntriesMetadata_accessed(subDirs);
            flatbuffers_uint64_vec_t last_modified_vector = BlorgMetaFlat_DirectoryEntriesMetadata_modified(subDirs);

            for (size_t i = 0; i < directoryCount; ++i)
            {
                flatbuffers_string_t name = flatbuffers_string_vec_at(name_vector, i);
                uint64_t size = flatbuffers_uint64_vec_at(size_vector, i);
                uint64_t accessed = flatbuffers_uint64_vec_at(last_accessed_vector, i);
                uint64_t modified = flatbuffers_uint64_vec_at(last_modified_vector, i);

                (void)name;
                (void)size;
                (void)accessed;
                (void)modified;

                BLORGFS_PRINT("Querying file:%llu -- name=%s -- size=%llu -- accessed=%llu -- modified=%llu\n", i, name, size, accessed, modified);
            }

            KdBreakPoint();
            BLORGFS_PRINT("subDirs worked!\n");

            //todo

        }
    }

    if (allocated)
    {
        ExFreePool(buffer);
    }

    return STATUS_SUCCESS;
}


NTSTATUS GetHttpDirectoryInfo(const PANSI_STRING path)
{
    KdBreakPoint();

    char sendBufferFormat[] =
        "GET /get_dir_info?directory=%Z HTTP/1.1\r\n"
        "Connection: keep-alive\r\n"
        "Host: blorgfs-server.blorg.lan:8080\r\n"
        "\r\n";

    NTSTATUS result = STATUS_SUCCESS;

    ULONG sendBufferSize = sizeof(sendBufferFormat) + path->Length;
    char* sendBuffer = ExAllocatePoolZero(PagedPool, sendBufferSize, 'BOOB');

    if (!sendBuffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    result = RtlStringCbPrintfA(sendBuffer, sendBufferSize, sendBufferFormat, path);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT(" BlorgGetDirectoryInfo() - No send buffer alloc");
        ExFreePool(sendBuffer);
        return result;
    }

    ULONG receiveBufferSize = 10'000;
    char* receiveBuffer = ExAllocatePoolZero(PagedPool, receiveBufferSize, 'TEST');

    if (!receiveBuffer)
    {
        BLORGFS_PRINT(" BlorgGetDirectoryInfo() - No receive buffer alloc");
        ExFreePool(sendBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PKSOCKET pSocket;
    result = CreateWskSocket(&pSocket, SOCK_STREAM, IPPROTO_TCP, 0, global.RemoteAddressInfo->ai_addr);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT(" BlorgGetDirectoryInfo() - Failed to create socket");
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = SendRecvWsk(pSocket, sendBuffer, sendBufferSize, NULL, 0, TRUE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT(" BlorgGetDirectoryInfo() - Failed to send");
        CloseWskSocket(pSocket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    ULONG bytesWritten;
    result = SendRecvWsk(pSocket, receiveBuffer, receiveBufferSize, &bytesWritten, 0, FALSE);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT(" BlorgGetDirectoryInfo() - Failed to receive");
        CloseWskSocket(pSocket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    int minor_version;
    int status;
    const char* msg;
    SIZE_T msg_len;
    struct phr_header headers[4];
    SIZE_T num_headers = sizeof(headers) / sizeof(headers[0]);


    int bytesProcessed = phr_parse_response(receiveBuffer, receiveBufferSize, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

    if (0 > bytesProcessed || 200 != status)
    {
        BLORGFS_PRINT(" phr_parse_response() - Failed to parse");
        CloseWskSocket(pSocket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    result = DeserializeFlatBuffer(headers, num_headers, (receiveBuffer + bytesProcessed), ((SIZE_T)receiveBufferSize - bytesProcessed));

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT(" DeserializeFlatBuffer() - Failed to deserialise");
        CloseWskSocket(pSocket);
        ExFreePool(receiveBuffer);
        ExFreePool(sendBuffer);
        return result;
    }

    result = CloseWskSocket(pSocket);
    ExFreePool(receiveBuffer);
    ExFreePool(sendBuffer);

    return result;
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
