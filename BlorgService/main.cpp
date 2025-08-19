#include "cpp-httplib/httplib.h"

#include "generated/metadata_flatbuffer_generated.h"

#include "..\BlorgFSCTL.h"

constexpr DWORD MAX_REQUESTS = 8;

constexpr std::string_view HOST_NAME = "blorgfs-server.blorg.lan";
constexpr int PORT = 8080;

struct OVL_WRAPPER
{
    OVERLAPPED  Overlapped;
    ULONG ResponseBufferLength;
    BLORGFS_TRANSACT TransactionData;
};

using OvlWrapperPtr = std::unique_ptr<OVL_WRAPPER, void(*)(void*)>;

HANDLE driverHandle;
PTP_IO pTpIo;

std::shared_ptr<httplib::Client> httpClient;

inline void InitializeClient()
{
    httpClient = std::make_shared<httplib::Client>(HOST_NAME.data(), PORT);
    httpClient->set_connection_timeout(5);
    httpClient->set_read_timeout(10);
    httpClient->set_keep_alive(true);
}

inline OvlWrapperPtr CreateWrapper(const BLORGFS_TRANSACT& TransactionData, ULONG ResponseBufferLength, NTSTATUS Status = 0)
{
    auto wrapper = OvlWrapperPtr(static_cast<OVL_WRAPPER*>(calloc(1, UFIELD_OFFSET(OVL_WRAPPER, TransactionData.Payload.ResponseBuffer) + ResponseBufferLength)), free);
    
    if (!wrapper)
    {
        printf("Failed to allocate memory for transaction wrapper\n");
        throw std::bad_alloc();
    }

    wrapper->TransactionData.RequestId = TransactionData.RequestId;

    wrapper->TransactionData.Payload.Status = Status;

    wrapper->ResponseBufferLength = ResponseBufferLength;

    return wrapper;
}

inline DWORD CreateInvertedCallTransaction(OvlWrapperPtr& Wrap)
{
    StartThreadpoolIo(pTpIo);

    BOOL result = DeviceIoControl(driverHandle,
        static_cast<DWORD>(FSCTL_BLORGFS_TRANSACT),
        nullptr,
        0,
        &Wrap->TransactionData,
        UFIELD_OFFSET(BLORGFS_TRANSACT, Payload.ResponseBuffer) + Wrap->ResponseBufferLength,
        nullptr,
        &Wrap->Overlapped);

    DWORD code = GetLastError();

    if (result)
    {
        CancelThreadpoolIo(pTpIo);

        return ERROR_SUCCESS;
    }

    if (code == ERROR_IO_PENDING)
    {
        Wrap.release(); // wrap ownership to the thread pool I/O
        return code;
    }
    else
    {
        // Operation failed immediately
        printf("DeviceIoControl failed with error 0x%lx\n", code);
        // Cancel the thread pool I/O since the operation failed
        CancelThreadpoolIo(pTpIo);
        return code;
    }
}

inline std::string ConvertWideStringToUTF8(const std::wstring_view& WideStr)
{
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, WideStr.data(), static_cast<int>(WideStr.length()), nullptr, 0, nullptr, nullptr);
    std::string pathUtf8(utf8Len, '\0'); // -1 to exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, WideStr.data(), static_cast<int>(WideStr.length()), pathUtf8.data(), utf8Len, nullptr, nullptr);
    return pathUtf8;
}

NTSTATUS ParseDirectoryInfo(const BlorgMetaFlat::Directory* const Directory, OvlWrapperPtr& Wrap)
{
    size_t headerSize = sizeof(DIRECTORY_INFO);
    size_t fileCount = Directory->files()->size();
    size_t subDirCount = Directory->subdirectories()->size();
    size_t fileEntrySize = fileCount * sizeof(DIRECTORY_FILE_METADATA);

    auto dirInfo = reinterpret_cast<PDIRECTORY_INFO>(Wrap->TransactionData.Payload.ResponseBuffer);

    dirInfo->FilesOffset = headerSize;
    dirInfo->SubDirsOffset = headerSize + fileEntrySize;
    dirInfo->FileCount = fileCount;
    dirInfo->SubDirCount = subDirCount;

    // base of the file entries
    auto fileEntries = GetFileEntry(dirInfo, 0);

    for (size_t i = 0; i < dirInfo->FileCount; ++i)
    {
        auto flatFileEntry = Directory->files()->Get(static_cast<flatbuffers::uoffset_t>(i));

        if (!flatFileEntry)
        {
            printf("Failed to destructure file flatbuffer\n");
            return STATUS_INVALID_PARAMETER;
        }

        fileEntries[i].Size = flatFileEntry->size();
        fileEntries[i].CreationTime = flatFileEntry->created();
        fileEntries[i].LastAccessedTime = flatFileEntry->accessed();
        fileEntries[i].LastModifiedTime = flatFileEntry->modified();

        // Convert name to wide string
        const char* name = flatFileEntry->name()->c_str();

        int result = MultiByteToWideChar(CP_UTF8, 0, name, -1,
            fileEntries[i].Name, MAX_NAME_LEN);

        if (result == 0)
        {
            printf("Failed to convert file name to wide string\n");
            return STATUS_INVALID_PARAMETER;
        }

        fileEntries[i].NameLength = result - 1; // Exclude null terminator
    }

    // base of the directory entries
    auto subDirEntries = GetSubDirEntry(dirInfo, 0);

    for (size_t i = 0; i < dirInfo->SubDirCount; ++i)
    {
        auto flatSubdirEntry = Directory->subdirectories()->Get(static_cast<flatbuffers::uoffset_t>(i));

        if (!flatSubdirEntry)
        {
            printf("Failed to destructure subdirectory flatbuffer\n");
            return STATUS_INVALID_PARAMETER;
        }

        subDirEntries[i].CreationTime = flatSubdirEntry->created();
        subDirEntries[i].LastAccessedTime = flatSubdirEntry->accessed();
        subDirEntries[i].LastModifiedTime = flatSubdirEntry->modified();

        // Convert name to wide string
        const char* name = flatSubdirEntry->name()->c_str();
        
        int result = MultiByteToWideChar(CP_UTF8, 0, name, -1,
            subDirEntries[i].Name, MAX_NAME_LEN);
        
        if (result == 0)
        {
            printf("Failed to convert subdirectory name to wide string\n");
            return STATUS_INVALID_PARAMETER;
        }
        
        subDirEntries[i].NameLength = result - 1; // Exclude null terminator
    }

    return 0;
}

OvlWrapperPtr ReadFileHandler(const OvlWrapperPtr& CurrentWrap)
{
    std::wstring_view pathView(CurrentWrap->TransactionData.PathBuffer, CurrentWrap->TransactionData.PathLength / sizeof(WCHAR));

    std::string getRequest = "/get_file?path=" + httplib::encode_uri_component(ConvertWideStringToUTF8(pathView));

    httplib::Headers headers;

    headers.emplace("Range", "bytes=" + std::to_string(CurrentWrap->TransactionData.Context.ReadFile.StartOffset) + "-" + std::to_string((CurrentWrap->TransactionData.Context.ReadFile.StartOffset + CurrentWrap->TransactionData.Context.ReadFile.Length) - 1));

    auto res = httpClient->Get(getRequest, headers);

    if (!res || res->status != 206)
    {
        printf("Http get for get_file failed\n");

        return CreateWrapper(CurrentWrap->TransactionData, 0, STATUS_INVALID_PARAMETER);
    }

    auto newWrap = CreateWrapper(CurrentWrap->TransactionData, static_cast<ULONG>(res->body.size()));
    
    memcpy(newWrap->TransactionData.Payload.ResponseBuffer, res->body.data(), res->body.size());

    return newWrap;
}

OvlWrapperPtr QueryFileHandler(const OvlWrapperPtr& CurrentWrap)
{
    std::wstring_view pathView(CurrentWrap->TransactionData.PathBuffer, CurrentWrap->TransactionData.PathLength / sizeof(WCHAR));

    std::string getRequest = "/get_dir_entry_info?path=" + httplib::encode_uri_component(ConvertWideStringToUTF8(pathView));

    auto res = httpClient->Get(getRequest);

    if (!res || res->status != 200)
    {
        printf("Http get for get_dir_entry_info failed\n");
        return CreateWrapper(CurrentWrap->TransactionData, 0, STATUS_INVALID_PARAMETER);
    }

    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(res->body.data()), res->body.size());

    bool ok = verifier.VerifyBuffer<BlorgMetaFlat::DirectoryEntryMetadata>(nullptr);

    if (!ok)
    {
        printf("Malformed serialized directory entry\n");
        return CreateWrapper(CurrentWrap->TransactionData, 0, STATUS_INVALID_PARAMETER);
    }

    auto directoryEntry = flatbuffers::GetRoot<BlorgMetaFlat::DirectoryEntryMetadata>(reinterpret_cast<const uint8_t*>(res->body.data()));

    auto newWrap = CreateWrapper(CurrentWrap->TransactionData, sizeof(DIRECTORY_ENTRY_METADATA));

    auto dirEntryMeta = reinterpret_cast<PDIRECTORY_ENTRY_METADATA>(newWrap->TransactionData.Payload.ResponseBuffer);

    dirEntryMeta->CreationTime = directoryEntry->created();
    dirEntryMeta->LastModifiedTime = directoryEntry->modified();
    dirEntryMeta->Size = directoryEntry->size();
    dirEntryMeta->IsDirectory = directoryEntry->directory();
    dirEntryMeta->LastAccessedTime = directoryEntry->accessed();

    return newWrap;
}

OvlWrapperPtr ListDirectoryHandler(const OvlWrapperPtr& CurrentWrap)
{
    std::wstring_view pathView(CurrentWrap->TransactionData.PathBuffer, CurrentWrap->TransactionData.PathLength / sizeof(WCHAR));

    std::string getRequest = "/get_dir_info?path=" + httplib::encode_uri_component(ConvertWideStringToUTF8(pathView));

    auto res = httpClient->Get(getRequest);

    if (!res || res->status != 200)
    {
        printf("Http get for get_dir_info failed\n");
        return CreateWrapper(CurrentWrap->TransactionData, 0, STATUS_INVALID_PARAMETER);
    }

    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(res->body.data()), res->body.size());

    bool ok = verifier.VerifyBuffer<BlorgMetaFlat::Directory>(nullptr);

    if (!ok)
    {
        printf("Malformed serialized directory listing\n");
        return CreateWrapper(CurrentWrap->TransactionData, 0, STATUS_INVALID_PARAMETER);
    }

    auto directory = flatbuffers::GetRoot<BlorgMetaFlat::Directory>(reinterpret_cast<const uint8_t*>(res->body.data()));

    size_t headerSize = sizeof(DIRECTORY_INFO);
    size_t fileEntrySize = directory->files()->size() * sizeof(DIRECTORY_FILE_METADATA);
    size_t subDirEntrySize = directory->subdirectories()->size() * sizeof(DIRECTORY_SUBDIR_METADATA);
    size_t totalSize = headerSize + fileEntrySize + subDirEntrySize;

    auto newWrap = CreateWrapper(CurrentWrap->TransactionData, static_cast<ULONG>(totalSize));

    newWrap->TransactionData.Payload.Status = ParseDirectoryInfo(directory, newWrap);

    return newWrap;
}

VOID CALLBACK IoCompletionCallback(
    PTP_CALLBACK_INSTANCE Instance,
    PVOID Context,
    PVOID Overlapped,
    ULONG IoResult,
    ULONG_PTR NumberOfBytesTransferred,
    PTP_IO Io
)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Io);

    auto wrapper = OvlWrapperPtr(
        static_cast<OVL_WRAPPER*>(Overlapped),
        free
    );

    if (!wrapper)
    { 
        return;
    }

    if (IoResult == NO_ERROR)
    {
        if (NumberOfBytesTransferred == 0)
        {
            // Handle case where no bytes were transferred
            printf("I/O completed with no data transferred\n");
        }
        else
        {
            // Process the completed I/O operation
            switch (wrapper->TransactionData.InvertedCallType)
            {
                case InvertedCallTypeListDirectory:
                {
                    wrapper = ListDirectoryHandler(wrapper);

                   break;
                }
                case InvertedCallTypeQueryFile:
                {
                    wrapper = QueryFileHandler(wrapper);
                    
                    break;
                }
                case InvertedCallTypeReadFile:
                {
                    wrapper = ReadFileHandler(wrapper);

                    break;
                }
                default:
                {
                    break;
                }
            }

            printf("I/O completed successfully, %llu bytes transferred\n",
                NumberOfBytesTransferred);
        }
    }
    else
    {
        printf("I/O completed with error 0x%lx\n", IoResult);
    }

    DWORD errorCode = CreateInvertedCallTransaction(wrapper);

    // deal with potential outstanding requests
    while (ERROR_SUCCESS == errorCode)
    {
        // Process the completed I/O operation
        switch (wrapper->TransactionData.InvertedCallType)
        {
            case InvertedCallTypeListDirectory:
            {
                wrapper = ListDirectoryHandler(wrapper);

                break;
            }
            case InvertedCallTypeQueryFile:
            {
                wrapper = QueryFileHandler(wrapper);

                break;
            }
            case InvertedCallTypeReadFile:
            {
                wrapper = ReadFileHandler(wrapper);

                break;
            }
            default:
            {
                memset(&wrapper->Overlapped, 0, sizeof(OVERLAPPED));
                break;
            }
        }

        errorCode = CreateInvertedCallTransaction(wrapper);
    }
}

int main()
{
    DWORD code;

    InitializeClient();

    driverHandle = CreateFile(L"\\\\.\\Global\\BlorgFS",
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (driverHandle == INVALID_HANDLE_VALUE)
    {
        code = GetLastError();
        printf("CreateFile failed with error 0x%lx\n", code);
        return(code);
    }

    pTpIo = CreateThreadpoolIo(driverHandle,
        IoCompletionCallback,
        nullptr,
        nullptr);

    if (pTpIo == nullptr)
    {
        code = GetLastError();
        printf("CreateThreadpoolIo failed with error 0x%lx\n", code);
        CloseHandle(driverHandle);
        return(code);
    }

    printf("Thread pool I/O initialized. Starting operations...\n");

    for  (int i = 0; i < MAX_REQUESTS; ++i)
    {
        OvlWrapperPtr wrapper(static_cast<OVL_WRAPPER*>(calloc(1, UFIELD_OFFSET(OVL_WRAPPER, TransactionData.Payload.ResponseBuffer))), free);
        
        if (!wrapper)
        {
            printf("Failed to allocate memory for wrapper\n");
            throw std::bad_alloc();
        }

        code = CreateInvertedCallTransaction(wrapper);

        // deal with potential outstanding requests
        while (ERROR_SUCCESS == code)
        {
            switch (wrapper->TransactionData.InvertedCallType)
            {
                case InvertedCallTypeListDirectory:
                {
                    wrapper = ListDirectoryHandler(wrapper);

                    break;
                }
                case InvertedCallTypeQueryFile:
                {
                    wrapper = QueryFileHandler(wrapper);

                    break;
                }
                case InvertedCallTypeReadFile:
                {
                    wrapper = ReadFileHandler(wrapper);

                    break;
                }
                default:
                {
                    memset(&wrapper->Overlapped, 0, sizeof(OVERLAPPED));
                    break;
                }
            }

            code = CreateInvertedCallTransaction(wrapper);
        }
    }

    while (1)
    {
        Sleep(5000); // Keep main thread alive to allow I/O callbacks to process
    }

    // Cleanup
    if (pTpIo)
    {
        WaitForThreadpoolIoCallbacks(pTpIo, FALSE);
        CloseThreadpoolIo(pTpIo);
    }

    CloseHandle(driverHandle);
    return 0;
}