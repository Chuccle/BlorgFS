//
// Coverage for the real FileInfo.c and VolumeInfo.c: every FILE_XXX_INFORMATION
// and FILE_FS_XXX_INFORMATION class the driver fills from the in-memory
// FCB/DCB or reports statically, plus the dispatch-entry device-type
// routing and buffer-size validation shared across them. Both files were
// previously 0% -- unlike Create.c/Read.c, neither touches the network,
// or the cache manager, so these tests build a bare FCB/DCB
// and drive the real dispatch entry points directly.
//

#include <gtest/gtest.h>

#include <cwchar>
#include <memory>
#include <vector>

extern "C" {
#include "..\..\src\Driver.h"
}

#include "DeviceKindScope.h"

namespace
{

const ULONGLONG kFileSize = 12345;

class FileInfoTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);
        global.VolumeDeviceObject = Volume;

        memset(&DiskDevice, 0, sizeof(DiskDevice));
        DiskDevice.DeviceType = FILE_DEVICE_DISK;

        //
        // Arbitrary and deliberately not any real FILE_* characteristic:
        // BlorgVolumeQueryVolumeInformation copies this field through
        // verbatim, so the assertion is a round trip, and a recognisable
        // sentinel makes that intent obvious where a mirrored real
        // constant would look like a second definition free to drift.
        //
        DiskDevice.Characteristics = 0x5AA5;
        global.DiskDeviceObject = &DiskDevice;

        UNICODE_STRING fileName = Path(L"\\media\\clip.bin");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Fcb, (CSHORT)BLORGFS_FCB_SIGNATURE, &fileName, Volume, kFileSize));
        InitializeListHead(&Fcb->Links);
        Fcb->CreationTime = 100;
        Fcb->LastAccessedTime = 200;
        Fcb->LastModifiedTime = 300;

        UNICODE_STRING dirName = Path(L"\\media");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateDCB(&Dcb, (CSHORT)BLORGFS_DCB_SIGNATURE, &dirName, Volume));
        InitializeListHead(&Dcb->Links);
        Dcb->CreationTime = 400;
        Dcb->LastAccessedTime = 500;
        Dcb->LastModifiedTime = 600;
    }

    void TearDown() override
    {
        global.VolumeDeviceObject = nullptr;

        BlorgFreeFileContext(Fcb, Volume);
        BlorgFreeFileContext(Dcb, Volume);
        StructsModelDestroyVolume(Volume);

        KmAssertQuiescent("FileInfoTest teardown");
    }

    static UNICODE_STRING Path(const wchar_t* path)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(path);
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    //
    // One real QUERY_INFORMATION/QUERY_VOLUME_INFORMATION IRP. SystemBuffer
    // points straight at a caller-owned byte array (METHOD_BUFFERED-style,
    // matching how the real I/O manager hands these down) rather than
    // going through an MDL.
    //
    struct QueryRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
    };

    //
    // Heap-stable and owned by the fixture (never returned by value): the
    // IRP and stack location self-reference &FileObject, and a struct
    // holding pointers into its own members goes stale the moment it is
    // copied -- NRVO eliding that copy is an optimisation the compiler is
    // free to skip, not a guarantee, and this Debug build does skip it.
    //
    QueryRequest* PrepareFileQuery(PVOID context, FILE_INFORMATION_CLASS infoClass,
        PVOID buffer, ULONG length)
    {
        Requests.push_back(std::make_unique<QueryRequest>());
        QueryRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->FileObject.FsContext = context;
        req->FileObject.DeviceObject = Volume;
        req->Stack.MajorFunction = IRP_MJ_QUERY_INFORMATION;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.DeviceObject = Volume;
        req->Stack.Parameters.QueryFile.FileInformationClass = infoClass;
        req->Stack.Parameters.QueryFile.Length = length;
        req->Irp.StackLocation = &req->Stack;
        req->Irp.AssociatedIrp.SystemBuffer = buffer;
        return req;
    }

    QueryRequest* PrepareVolumeQuery(FS_INFORMATION_CLASS infoClass, PVOID buffer, ULONG length)
    {
        Requests.push_back(std::make_unique<QueryRequest>());
        QueryRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->FileObject.FsContext = Fcb;
        req->FileObject.DeviceObject = Volume;
        req->Stack.MajorFunction = IRP_MJ_QUERY_VOLUME_INFORMATION;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.DeviceObject = Volume;
        req->Stack.Parameters.QueryVolume.FsInformationClass = infoClass;
        req->Stack.Parameters.QueryVolume.Length = length;
        req->Irp.StackLocation = &req->Stack;
        req->Irp.AssociatedIrp.SystemBuffer = buffer;
        return req;
    }

    PDEVICE_OBJECT Volume = nullptr;
    PFCB Fcb = nullptr;
    PDCB Dcb = nullptr;
    std::vector<std::unique_ptr<QueryRequest>> Requests;
    DEVICE_OBJECT DiskDevice{};
};

///////////////////////////////////////////////////////////////////////////
// BlorgQueryInformation / BlorgVolumeQueryInformation
///////////////////////////////////////////////////////////////////////////

TEST_F(FileInfoTest, PositionInformationReportsCurrentByteOffset)
{
    FILE_POSITION_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Fcb, FilePositionInformation, &buffer, sizeof(buffer));
    req->FileObject.CurrentByteOffset.QuadPart = 42;

    EXPECT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(42, buffer.CurrentByteOffset.QuadPart);
    EXPECT_EQ((ULONG_PTR)sizeof(buffer), req->Irp.IoStatus.Information);
}

TEST_F(FileInfoTest, PositionInformationTooSmallBufferIsRejected)
{
    FILE_POSITION_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Fcb, FilePositionInformation, &buffer, 2);

    EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, BlorgQueryInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, NameInformationCopiesTheFullPath)
{
    unsigned char storage[128] = {};
    auto* buffer = reinterpret_cast<PFILE_NAME_INFORMATION>(storage);
    QueryRequest* req = PrepareFileQuery(Fcb, FileNameInformation, buffer, sizeof(storage));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(Fcb->FullPath.Length, buffer->FileNameLength);
    EXPECT_EQ(0, memcmp(buffer->FileName, Fcb->FullPath.Buffer, Fcb->FullPath.Length));
}

TEST_F(FileInfoTest, NameInformationTooSmallForTheNameOverflows)
{
    unsigned char storage[sizeof(FILE_NAME_INFORMATION)] = {};
    auto* buffer = reinterpret_cast<PFILE_NAME_INFORMATION>(storage);
    QueryRequest* req = PrepareFileQuery(Fcb, FileNameInformation, buffer, sizeof(storage));

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, req->Irp.IoStatus.Information)
        << "an overflowing name must report zero bytes written, not a partial copy";
}

TEST_F(FileInfoTest, BasicInformationReportsFileAttributesForAFile)
{
    FILE_BASIC_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Fcb, FileBasicInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(100, buffer.CreationTime.QuadPart);
    EXPECT_EQ(200, buffer.LastAccessTime.QuadPart);
    EXPECT_EQ(300, buffer.LastWriteTime.QuadPart);
    EXPECT_EQ((ULONG)FILE_ATTRIBUTE_NORMAL, buffer.FileAttributes);
}

TEST_F(FileInfoTest, BasicInformationReportsDirectoryAttributeForADcb)
{
    FILE_BASIC_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Dcb, FileBasicInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ((ULONG)FILE_ATTRIBUTE_DIRECTORY, buffer.FileAttributes);
}

TEST_F(FileInfoTest, StandardInformationReportsSizeAndDirectoryFlag)
{
    FILE_STANDARD_INFORMATION fileBuffer{};
    QueryRequest* fileReq = PrepareFileQuery(Fcb, FileStandardInformation, &fileBuffer, sizeof(fileBuffer));
    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &fileReq->Irp));
    EXPECT_EQ((LONGLONG)kFileSize, fileBuffer.EndOfFile.QuadPart);
    EXPECT_FALSE(fileBuffer.Directory);

    FILE_STANDARD_INFORMATION dirBuffer{};
    QueryRequest* dirReq = PrepareFileQuery(Dcb, FileStandardInformation, &dirBuffer, sizeof(dirBuffer));
    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &dirReq->Irp));
    EXPECT_TRUE(dirBuffer.Directory);
}

TEST_F(FileInfoTest, EaInformationReportsZeroEaSize)
{
    FILE_EA_INFORMATION buffer{ 0xFFFFFFFF };
    QueryRequest* req = PrepareFileQuery(Fcb, FileEaInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, buffer.EaSize);
}

TEST_F(FileInfoTest, AttributeTagInformationMatchesBasicInformation)
{
    FILE_ATTRIBUTE_TAG_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Dcb, FileAttributeTagInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ((ULONG)FILE_ATTRIBUTE_DIRECTORY, buffer.FileAttributes);
}

TEST_F(FileInfoTest, NetworkOpenInformationReportsTimesAndSize)
{
    FILE_NETWORK_OPEN_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Fcb, FileNetworkOpenInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(100, buffer.CreationTime.QuadPart);
    EXPECT_EQ((LONGLONG)kFileSize, buffer.EndOfFile.QuadPart);
}

//
// The existing size assertions above cannot catch a class reading the wrong
// field, because BlorgCreateFCB sets FileSize and AllocationSize equal, so
// both answers look right. This one drives them apart first.
//
// FileNetworkOpenInformation filled EndOfFile from AllocationSize, so it
// and FileStandardInformation could report a different end-of-file for the
// same file the moment the two stopped agreeing -- and it is
// FileNetworkOpenInformation that the loader and Explorer take as the fast
// path, so it is the one whose answer gets acted on. Both classes are
// queried here against the same FCB, and the assertion is that they agree
// with each other and with FileSize.
//
TEST_F(FileInfoTest, EndOfFileComesFromFileSizeNotAllocationSize)
{
    const LONGLONG allocation = (LONGLONG)kFileSize * 4;

    Fcb->Header.AllocationSize.QuadPart = allocation;

    FILE_NETWORK_OPEN_INFORMATION networkOpen{};
    QueryRequest* networkReq = PrepareFileQuery(Fcb, FileNetworkOpenInformation, &networkOpen, sizeof(networkOpen));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &networkReq->Irp));

    FILE_STANDARD_INFORMATION standard{};
    QueryRequest* standardReq = PrepareFileQuery(Fcb, FileStandardInformation, &standard, sizeof(standard));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &standardReq->Irp));

    EXPECT_EQ((LONGLONG)kFileSize, networkOpen.EndOfFile.QuadPart)
        << "FileNetworkOpenInformation reported the allocation size as end-of-file";
    EXPECT_EQ(standard.EndOfFile.QuadPart, networkOpen.EndOfFile.QuadPart)
        << "two classes disagreed about the end of the same file";

    EXPECT_EQ(allocation, networkOpen.AllocationSize.QuadPart)
        << "the allocation size itself must still be reported as such";

    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)kFileSize;
}

TEST_F(FileInfoTest, AllInformationSucceedsWhenTheNameFits)
{
    unsigned char storage[512] = {};
    auto* buffer = reinterpret_cast<PFILE_ALL_INFORMATION>(storage);
    QueryRequest* req = PrepareFileQuery(Fcb, FileAllInformation, buffer, sizeof(storage));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ((LONGLONG)kFileSize, buffer->StandardInformation.EndOfFile.QuadPart);
    EXPECT_EQ(Fcb->FullPath.Length, buffer->NameInformation.FileNameLength);
    EXPECT_EQ(0, memcmp(buffer->NameInformation.FileName, Fcb->FullPath.Buffer, Fcb->FullPath.Length));
}

//
// A buffer that fits FILE_ALL_INFORMATION's fixed part but not the whole
// name must still report the fixed fields and the true name length,
// truncating only the copied name bytes -- the caller's contract for
// BUFFER_OVERFLOW on a variable-length trailer.
//
TEST_F(FileInfoTest, AllInformationOverflowsWhenTheNameDoesNotFitButFillsTheFixedPart)
{
    ULONG baseLength = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName);
    ULONG length = baseLength + 2;
    std::vector<unsigned char> storage(length, 0xCD);
    auto* buffer = reinterpret_cast<PFILE_ALL_INFORMATION>(storage.data());

    QueryRequest* req = PrepareFileQuery(Fcb, FileAllInformation, buffer, length);

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ((LONGLONG)kFileSize, buffer->StandardInformation.EndOfFile.QuadPart)
        << "the fixed-size sections must still be filled on a name overflow";
    EXPECT_EQ(Fcb->FullPath.Length, buffer->NameInformation.FileNameLength)
        << "the true name length must be reported even when truncated";
    EXPECT_EQ((ULONG_PTR)(baseLength + 2), req->Irp.IoStatus.Information);
}

TEST_F(FileInfoTest, StandardLinkInformationReportsOneLink)
{
    FILE_STANDARD_LINK_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Dcb, FileStandardLinkInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(1u, buffer.TotalNumberOfLinks);
    EXPECT_TRUE(buffer.Directory);
}

TEST_F(FileInfoTest, CaseSensitiveInformationReportsNoFlags)
{
    FILE_CASE_SENSITIVE_INFORMATION buffer{ 0xFFFFFFFF };
    QueryRequest* req = PrepareFileQuery(Fcb, FileCaseSensitiveInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, buffer.Flags);
}

TEST_F(FileInfoTest, RemoteProtocolInformationIsAlwaysRejected)
{
    unsigned char buffer[64] = {};
    QueryRequest* req = PrepareFileQuery(Fcb, FileRemoteProtocolInformation, buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgQueryInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, UnhandledInformationClassIsRejectedWithNoBytesWritten)
{
    unsigned char buffer[256] = {};
    QueryRequest* req = PrepareFileQuery(Fcb, (FILE_INFORMATION_CLASS)999, buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgQueryInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
}

TEST_F(FileInfoTest, NonVolumeDeviceReturnsInvalidDeviceRequest)
{
    PDEVICE_OBJECT diskDevice = StructsModelCreateVolume();
    ASSERT_NE(nullptr, diskDevice);
    ScopedDeviceKind asDisk(&global.DiskDeviceObject, diskDevice);

    FILE_POSITION_INFORMATION buffer{};
    QueryRequest* req = PrepareFileQuery(Fcb, FilePositionInformation, &buffer, sizeof(buffer));
    req->Stack.DeviceObject = diskDevice;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgQueryInformation(diskDevice, &req->Irp));

    StructsModelDestroyVolume(diskDevice);
}

TEST_F(FileInfoTest, SetInformationIsAlwaysUnsupported)
{
    IRP irp{};
    IO_STACK_LOCATION stack{};
    irp.StackLocation = &stack;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgSetInformation(Volume, &irp))
        << "the volume is read-only -- SetInformation has no implemented branch";
}

///////////////////////////////////////////////////////////////////////////
// BlorgQueryVolumeInformation / BlorgVolumeQueryVolumeInformation
///////////////////////////////////////////////////////////////////////////

TEST_F(FileInfoTest, FsVolumeInformationReportsLabelAndSerial)
{
    unsigned char storage[64] = {};
    auto* buffer = reinterpret_cast<PFILE_FS_VOLUME_INFORMATION>(storage);
    QueryRequest* req = PrepareVolumeQuery(FileFsVolumeInformation, buffer, sizeof(storage));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
    EXPECT_EQ(0x12345678u, buffer->VolumeSerialNumber);
    EXPECT_EQ(0, memcmp(buffer->VolumeLabel, L"BLORGDRIVE", buffer->VolumeLabelLength));
}

TEST_F(FileInfoTest, FsVolumeInformationTooSmallForLabelOverflows)
{
    unsigned char storage[sizeof(FILE_FS_VOLUME_INFORMATION)] = {};
    auto* buffer = reinterpret_cast<PFILE_FS_VOLUME_INFORMATION>(storage);
    QueryRequest* req = PrepareVolumeQuery(FileFsVolumeInformation, buffer, sizeof(storage));

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, BlorgQueryVolumeInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
}

TEST_F(FileInfoTest, FsSizeInformationReportsZeroedCapacity)
{
    FILE_FS_SIZE_INFORMATION buffer{};
    buffer.BytesPerSector = 0xFFFFFFFF;
    QueryRequest* req = PrepareVolumeQuery(FileFsSizeInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
    EXPECT_EQ(0u, buffer.BytesPerSector) << "capacity is unknown, not zero-by-accident";
}

TEST_F(FileInfoTest, FsDeviceInformationReflectsTheDiskDeviceObject)
{
    FILE_FS_DEVICE_INFORMATION buffer{};
    QueryRequest* req = PrepareVolumeQuery(FileFsDeviceInformation, &buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
    EXPECT_EQ((DEVICE_TYPE)FILE_DEVICE_DISK, buffer.DeviceType);
    EXPECT_EQ(DiskDevice.Characteristics, buffer.Characteristics)
        << "characteristics must be reported from the disk device object, not synthesized";
}

TEST_F(FileInfoTest, FsAttributeInformationReportsReadOnlyAndName)
{
    unsigned char storage[64] = {};
    auto* buffer = reinterpret_cast<PFILE_FS_ATTRIBUTE_INFORMATION>(storage);
    QueryRequest* req = PrepareVolumeQuery(FileFsAttributeInformation, buffer, sizeof(storage));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
    EXPECT_TRUE(BooleanFlagOn(buffer->FileSystemAttributes, FILE_READ_ONLY_VOLUME));
    EXPECT_EQ(0, memcmp(buffer->FileSystemName, L"BLORGFS", buffer->FileSystemNameLength));
}

//
// The volume must not claim a case sensitivity it does not have. Every name
// comparison in this driver is case-insensitive, and
// FileCaseSensitiveInformation (CaseSensitiveInformationReportsNoFlags,
// above) already says so -- the attribute flag was the one place that said
// otherwise, which left the two query classes contradicting each other. An
// application that believes the flag and stops normalizing case gets silent
// aliasing: two names it treats as distinct open the same file.
//
TEST_F(FileInfoTest, FsAttributeInformationDoesNotClaimCaseSensitiveSearch)
{
    unsigned char storage[64] = {};
    auto* buffer = reinterpret_cast<PFILE_FS_ATTRIBUTE_INFORMATION>(storage);
    QueryRequest* req = PrepareVolumeQuery(FileFsAttributeInformation, buffer, sizeof(storage));

    ASSERT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));

    EXPECT_FALSE(BooleanFlagOn(buffer->FileSystemAttributes, FILE_CASE_SENSITIVE_SEARCH))
        << "the volume advertised case-sensitive search while comparing case-insensitively";
    EXPECT_TRUE(BooleanFlagOn(buffer->FileSystemAttributes, FILE_CASE_PRESERVED_NAMES))
        << "names are still returned with their original case";
}

TEST_F(FileInfoTest, FsAttributeInformationTooSmallForNameOverflows)
{
    unsigned char storage[sizeof(FILE_FS_ATTRIBUTE_INFORMATION)] = {};
    auto* buffer = reinterpret_cast<PFILE_FS_ATTRIBUTE_INFORMATION>(storage);
    QueryRequest* req = PrepareVolumeQuery(FileFsAttributeInformation, buffer, sizeof(storage));

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, BlorgQueryVolumeInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, FsFullSizeInformationReportsZeroedCapacity)
{
    FILE_FS_FULL_SIZE_INFORMATION buffer{};
    QueryRequest* req = PrepareVolumeQuery(FileFsFullSizeInformation, &buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, FsFullSizeInformationExReportsZeroedCapacity)
{
    FILE_FS_FULL_SIZE_INFORMATION_EX buffer{};
    QueryRequest* req = PrepareVolumeQuery(FileFsFullSizeInformationEx, &buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_SUCCESS, BlorgQueryVolumeInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, UnhandledFsInformationClassIsRejected)
{
    unsigned char buffer[64] = {};
    QueryRequest* req = PrepareVolumeQuery(FileFsObjectIdInformation, buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgQueryVolumeInformation(Volume, &req->Irp));
}

TEST_F(FileInfoTest, VolumeInformationOnNonVolumeDeviceReturnsInvalidDeviceRequest)
{
    PDEVICE_OBJECT diskDevice = StructsModelCreateVolume();
    ASSERT_NE(nullptr, diskDevice);
    ScopedDeviceKind asDisk(&global.DiskDeviceObject, diskDevice);

    FILE_FS_SIZE_INFORMATION buffer{};
    QueryRequest* req = PrepareVolumeQuery(FileFsSizeInformation, &buffer, sizeof(buffer));
    req->Stack.DeviceObject = diskDevice;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgQueryVolumeInformation(diskDevice, &req->Irp));

    StructsModelDestroyVolume(diskDevice);
}

TEST_F(FileInfoTest, SetVolumeInformationIsAlwaysUnsupported)
{
    IRP irp{};
    IO_STACK_LOCATION stack{};
    irp.StackLocation = &stack;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgSetVolumeInformation(Volume, &irp))
        << "the volume is read-only -- FILE_READ_ONLY_VOLUME is reported honestly";
}

} // namespace
