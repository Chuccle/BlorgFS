//
// The driver's half of the wire contract it shares with server-rs
// (third_party/schemas/contract.json). server-rs checks the other half
// against a real socket; between them, a change on either side that the
// other has not also taken fails a test instead of a mounted volume.
//
// Everything asserted here comes from the generated contract tables rather
// than from literals in this file: the exact request bytes the driver must
// send, and golden FlatBuffer bodies -- encoded by flatc, not by this
// driver's flatcc -- with the values they must decode to. Each test names
// the behaviour it covers with a `contract: Bnn` tag;
// third_party/schemas/tools/check_traceability.py fails CI if a behaviour
// the driver is responsible for has none.
//
// The responses go through the real receive path (the shipping Client.c
// against the scripted peer), not straight into the deserializers, so the
// alignment slide, Content-Length handling and PASSIVE bounce are all on
// the path exactly as they are for a live server.
//

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "SandboxSocket.h"
}

#include "third_party/schemas/generated/blorg_contract_fixtures.h"

namespace
{

struct DirInfoResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PDIRECTORY_INFO Info = nullptr;
};

struct EntryResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    DIRECTORY_ENTRY_METADATA Entry = {};
};

struct ReadResult
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
};

DirInfoResult LastDirInfo;
EntryResult LastEntry;
ReadResult LastRead;

void OnDirInfo(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext)
{
    (void)CallerContext;

    LastDirInfo.Calls++;
    LastDirInfo.Status = Status;
    LastDirInfo.Info = DirInfo;
}

void OnEntry(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* Entry, PVOID CallerContext)
{
    (void)CallerContext;

    LastEntry.Calls++;
    LastEntry.Status = Status;

    if (Entry)
    {
        LastEntry.Entry = *Entry;
    }
}

void OnRead(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    (void)FileBuffer;
    (void)CallerContext;

    LastRead.Calls++;
    LastRead.Status = Status;
}

UNICODE_STRING MakePath(const wchar_t* literal)
{
    UNICODE_STRING path;
    path.Buffer = const_cast<wchar_t*>(literal);
    path.Length = (USHORT)(wcslen(literal) * sizeof(wchar_t));
    path.MaximumLength = path.Length;
    return path;
}

std::wstring EntryName(const WCHAR* Name, SIZE_T NameLength)
{
    return std::wstring(Name, NameLength);
}

class ContractTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SandboxInitialize();
        LastDirInfo = {};
        LastEntry = {};
        LastRead = {};
    }

    void TearDown() override
    {
        if (LastDirInfo.Info)
        {
            BlorgFreeHttpDirectoryInfo(LastDirInfo.Info);
            LastDirInfo.Info = nullptr;
        }

        if (Mdl)
        {
            ShimFreeMdl(Mdl);
            Mdl = nullptr;
        }

        SandboxDrainCompletions();
        ShimDrainWorkItems();
        BlorgCleanupWskClient();

        EXPECT_EQ(0u, ShimPoolOutstanding()) << "pool allocation(s) leaked";
    }

    void Drain()
    {
        SandboxDrainCompletions();
        ShimDrainWorkItems();
    }

    //
    // Scripts the peer to answer with Status and Body, framed the way
    // server-rs frames every response (behaviour B02).
    //
    void Respond(int Status, const void* Body, SIZE_T BodyLength)
    {
        std::string head = "HTTP/1.1 " + std::to_string(Status) + " X\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " + std::to_string(BodyLength) + "\r\n"
            "\r\n";

        Response.assign(head.begin(), head.end());
        Response.insert(Response.end(), (const unsigned char*)Body, (const unsigned char*)Body + BodyLength);

        Script[0] = { SandboxStepDeliver, Response.data(), Response.size(), STATUS_SUCCESS, TRUE };
        SandboxSetPeerScript(Script, 1);
    }

    void RespondRaw(const char* Raw)
    {
        Response.assign(Raw, Raw + strlen(Raw));
        Script[0] = { SandboxStepDeliver, Response.data(), Response.size(), STATUS_SUCCESS, TRUE };
        SandboxSetPeerScript(Script, 1);
    }

    NTSTATUS ReadRange(const wchar_t* Path, SIZE_T First, SIZE_T Length)
    {
        Target.assign(Length, 0);
        Mdl = ShimCreateMdl(Target.data(), Length);

        UNICODE_STRING path = MakePath(Path);
        return BlorgHttpGetFileMdl(&path, First, Length, Mdl, OnRead, nullptr);
    }

    std::string SentRequest()
    {
        SIZE_T length = 0;
        const unsigned char* sent = SandboxLastRequest(&length);
        return std::string((const char*)sent, length);
    }

    std::vector<unsigned char> Response;
    std::vector<unsigned char> Target;
    SANDBOX_STEP Script[1] = {};
    PMDL Mdl = nullptr;
};

const BLORG_CONTRACT_ENTRY* FindEntry(const char* Id)
{
    for (const BLORG_CONTRACT_ENTRY& entry : BlorgContractEntries)
    {
        if (0 == strcmp(entry.Id, Id))
        {
            return &entry;
        }
    }

    return nullptr;
}

const BLORG_CONTRACT_LISTING* FindListing(const char* Id)
{
    for (const BLORG_CONTRACT_LISTING& listing : BlorgContractListings)
    {
        if (0 == strcmp(listing.Id, Id))
        {
            return &listing;
        }
    }

    return nullptr;
}

} // namespace

///////////////////////////////////////////////////////////////////////////
// What the driver sends
///////////////////////////////////////////////////////////////////////////

//
// contract: B05
//
// Byte for byte, the request server-rs is tested against. The path is the
// one Windows hands the driver -- leading backslash, backslash separators,
// non-ASCII -- so this pins the percent-encoding and the request line
// together. A change to either is a contract change.
//
class ContractRequestTest : public ContractTest,
                            public ::testing::WithParamInterface<size_t>
{
};

TEST_P(ContractRequestTest, IsByteForByteTheContractsRequest)
{
    const BLORG_CONTRACT_REQUEST& request = BlorgContractRequests[GetParam()];
    UNICODE_STRING path = MakePath(request.Path);

    if (request.HasRange)
    {
        const SIZE_T length = (SIZE_T)(request.RangeLast - request.RangeFirst + 1);
        std::vector<unsigned char> body(length, 'x');

        Respond(BLORG_CONTRACT_STATUS_FILE_OK, body.data(), body.size());
        ASSERT_EQ(STATUS_PENDING, ReadRange(request.Path, (SIZE_T)request.RangeFirst, length));
    }
    else if (0 == strncmp(request.Wire, "GET " BLORG_CONTRACT_ROUTE_DIR_INFO "?", strlen("GET " BLORG_CONTRACT_ROUTE_DIR_INFO "?")))
    {
        const BLORG_CONTRACT_LISTING* empty = FindListing("LISTING_EMPTY");
        ASSERT_NE(nullptr, empty);

        Respond(BLORG_CONTRACT_STATUS_DIR_INFO_OK, empty->Bytes, empty->ByteCount);
        ASSERT_EQ(STATUS_PENDING, BlorgHttpGetDirectoryInfo(&path, OnDirInfo, nullptr));
    }
    else
    {
        const BLORG_CONTRACT_ENTRY* entry = FindEntry("ENTRY_FILE");
        ASSERT_NE(nullptr, entry);

        Respond(BLORG_CONTRACT_STATUS_DIR_ENTRY_INFO_OK, entry->Bytes, entry->ByteCount);
        ASSERT_EQ(STATUS_PENDING, BlorgHttpGetFileInformation(&path, OnEntry, nullptr));
    }

    Drain();

    EXPECT_EQ(std::string(request.Wire), SentRequest()) << request.Id;
    EXPECT_EQ(1, LastDirInfo.Calls + LastEntry.Calls + LastRead.Calls) << request.Id;
    EXPECT_TRUE(NT_SUCCESS(LastDirInfo.Status) && NT_SUCCESS(LastEntry.Status) && NT_SUCCESS(LastRead.Status))
        << request.Id << ": the contract's own response was not accepted";
}

INSTANTIATE_TEST_SUITE_P(
    Contract,
    ContractRequestTest,
    ::testing::Range<size_t>(0, RTL_NUMBER_OF(BlorgContractRequests)),
    [](const ::testing::TestParamInfo<size_t>& info) { return std::string(BlorgContractRequests[info.param].Id); });

///////////////////////////////////////////////////////////////////////////
// What the driver reads
///////////////////////////////////////////////////////////////////////////

//
// contract: B06 B07
//
// The golden listings decode, through flatcc and the driver's own
// conversion into DIRECTORY_INFO, to exactly the values the contract
// declares: names (including a supplementary-plane character, which is a
// surrogate pair on this side), u64 extremes in every field, and an empty
// directory that still carries both vectors.
//
class ContractListingTest : public ContractTest,
                            public ::testing::WithParamInterface<size_t>
{
};

TEST_P(ContractListingTest, DecodesToTheDeclaredValues)
{
    const BLORG_CONTRACT_LISTING& fixture = BlorgContractListings[GetParam()];
    UNICODE_STRING path = MakePath(L"\\");

    Respond(BLORG_CONTRACT_STATUS_DIR_INFO_OK, fixture.Bytes, fixture.ByteCount);
    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetDirectoryInfo(&path, OnDirInfo, nullptr));

    Drain();

    ASSERT_EQ(1, LastDirInfo.Calls);
    ASSERT_EQ(STATUS_SUCCESS, LastDirInfo.Status) << fixture.Id;
    ASSERT_NE(nullptr, LastDirInfo.Info);

    PDIRECTORY_INFO info = LastDirInfo.Info;

    ASSERT_EQ(fixture.FileCount, info->FileCount) << fixture.Id;
    for (SIZE_T i = 0; i < fixture.FileCount; ++i)
    {
        const BLORG_CONTRACT_FILE& want = fixture.Files[i];
        PDIRECTORY_FILE_METADATA got = BlorgGetFileEntry(info, i);

        ASSERT_NE(nullptr, got);
        EXPECT_EQ(std::wstring(want.Name), EntryName(got->Name, got->NameLength)) << fixture.Id << " file " << i;
        EXPECT_EQ(want.Size, got->Size) << fixture.Id << " file " << i;
        EXPECT_EQ(want.Created, got->CreationTime) << fixture.Id << " file " << i;
        EXPECT_EQ(want.Modified, got->LastModifiedTime) << fixture.Id << " file " << i;
        EXPECT_EQ(want.Accessed, got->LastAccessedTime) << fixture.Id << " file " << i;
    }

    ASSERT_EQ(fixture.SubDirCount, info->SubDirCount) << fixture.Id;
    for (SIZE_T i = 0; i < fixture.SubDirCount; ++i)
    {
        const BLORG_CONTRACT_SUBDIR& want = fixture.SubDirs[i];
        PDIRECTORY_SUBDIR_METADATA got = BlorgGetSubDirEntry(info, i);

        ASSERT_NE(nullptr, got);
        EXPECT_EQ(std::wstring(want.Name), EntryName(got->Name, got->NameLength)) << fixture.Id << " subdir " << i;
        EXPECT_EQ(want.Created, got->CreationTime) << fixture.Id << " subdir " << i;
        EXPECT_EQ(want.Modified, got->LastModifiedTime) << fixture.Id << " subdir " << i;
        EXPECT_EQ(want.Accessed, got->LastAccessedTime) << fixture.Id << " subdir " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Contract,
    ContractListingTest,
    ::testing::Range<size_t>(0, RTL_NUMBER_OF(BlorgContractListings)),
    [](const ::testing::TestParamInfo<size_t>& info) { return std::string(BlorgContractListings[info.param].Id); });

// contract: B07
class ContractEntryTest : public ContractTest,
                          public ::testing::WithParamInterface<size_t>
{
};

TEST_P(ContractEntryTest, DecodesToTheDeclaredValues)
{
    const BLORG_CONTRACT_ENTRY& fixture = BlorgContractEntries[GetParam()];
    UNICODE_STRING path = MakePath(L"\\entry");

    Respond(BLORG_CONTRACT_STATUS_DIR_ENTRY_INFO_OK, fixture.Bytes, fixture.ByteCount);
    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetFileInformation(&path, OnEntry, nullptr));

    Drain();

    ASSERT_EQ(1, LastEntry.Calls);
    ASSERT_EQ(STATUS_SUCCESS, LastEntry.Status) << fixture.Id;
    EXPECT_EQ(fixture.Size, LastEntry.Entry.Size) << fixture.Id;
    EXPECT_EQ(fixture.Created, LastEntry.Entry.CreationTime) << fixture.Id;
    EXPECT_EQ(fixture.Modified, LastEntry.Entry.LastModifiedTime) << fixture.Id;
    EXPECT_EQ(fixture.Accessed, LastEntry.Entry.LastAccessedTime) << fixture.Id;
    EXPECT_EQ(fixture.Directory != 0, LastEntry.Entry.IsDirectory != FALSE) << fixture.Id;
}

INSTANTIATE_TEST_SUITE_P(
    Contract,
    ContractEntryTest,
    ::testing::Range<size_t>(0, RTL_NUMBER_OF(BlorgContractEntries)),
    [](const ::testing::TestParamInfo<size_t>& info) { return std::string(BlorgContractEntries[info.param].Id); });

///////////////////////////////////////////////////////////////////////////
// What the driver makes of the server's answers
///////////////////////////////////////////////////////////////////////////

//
// contract: B01
//
// A 206 carrying exactly the requested range is the one success shape for
// a read, and its bytes land in the caller's buffer.
//
TEST_F(ContractTest, ExactRangeIsAccepted)
{
    static const char body[] = "ABCD";

    Respond(BLORG_CONTRACT_STATUS_FILE_OK, body, 4);
    ASSERT_EQ(STATUS_PENDING, ReadRange(L"\\media\\file.bin", 100, 4));

    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastRead.Status);
    EXPECT_EQ(0, memcmp(Target.data(), body, 4));
}

//
// contract: B04
//
// Every error status in the contract's table maps to the NTSTATUS the
// contract says the caller sees. 403 and 416 fell through to
// STATUS_INVALID_PARAMETER before this table existed.
//
struct StatusCase
{
    int HttpStatus;
    NTSTATUS Expected;
    const char* Name;
};

class ContractStatusTest : public ContractTest,
                           public ::testing::WithParamInterface<StatusCase>
{
};

TEST_P(ContractStatusTest, MapsToTheContractsNtStatus)
{
    Respond(GetParam().HttpStatus, "", 0);
    ASSERT_EQ(STATUS_PENDING, ReadRange(L"\\media\\file.bin", 0, 8));

    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_EQ(GetParam().Expected, LastRead.Status) << "HTTP " << GetParam().HttpStatus;
}

INSTANTIATE_TEST_SUITE_P(
    Contract,
    ContractStatusTest,
    ::testing::Values(
        StatusCase{ BLORG_CONTRACT_STATUS_NOT_FOUND, (NTSTATUS)STATUS_OBJECT_NAME_NOT_FOUND, "NotFound" },
        StatusCase{ BLORG_CONTRACT_STATUS_FORBIDDEN, (NTSTATUS)STATUS_ACCESS_DENIED, "Forbidden" },
        StatusCase{ BLORG_CONTRACT_STATUS_RANGE_NOT_SATISFIABLE, (NTSTATUS)STATUS_END_OF_FILE, "RangeNotSatisfiable" },
        StatusCase{ BLORG_CONTRACT_STATUS_BAD_REQUEST, (NTSTATUS)STATUS_INVALID_PARAMETER, "BadRequest" },
        StatusCase{ BLORG_CONTRACT_STATUS_INTERNAL, (NTSTATUS)STATUS_INVALID_PARAMETER, "Internal" }),
    [](const ::testing::TestParamInfo<StatusCase>& info) { return std::string(info.param.Name); });

//
// contract: B04
//
// The same mapping on a metadata request, where 403 is what a permission
// problem on the host (or a path the server refuses) looks like to an open.
//
TEST_F(ContractTest, ForbiddenListingIsAccessDenied)
{
    UNICODE_STRING path = MakePath(L"\\private");

    Respond(BLORG_CONTRACT_STATUS_FORBIDDEN, "", 0);
    ASSERT_EQ(STATUS_PENDING, BlorgHttpGetDirectoryInfo(&path, OnDirInfo, nullptr));

    Drain();

    ASSERT_EQ(1, LastDirInfo.Calls);
    EXPECT_EQ(STATUS_ACCESS_DENIED, LastDirInfo.Status);
    EXPECT_EQ(nullptr, LastDirInfo.Info);
}

//
// contract: B02
//
// Content-Length framing is the only framing the driver understands. A
// chunked response is refused outright rather than misread as a body of
// whatever happens to follow the headers.
//
TEST_F(ContractTest, ChunkedResponseIsRefused)
{
    RespondRaw(
        "HTTP/1.1 206 Partial Content\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nABCD\r\n0\r\n\r\n");

    ASSERT_EQ(STATUS_PENDING, ReadRange(L"\\media\\file.bin", 0, 4));

    Drain();

    ASSERT_EQ(1, LastRead.Calls);
    EXPECT_FALSE(NT_SUCCESS(LastRead.Status));
}
