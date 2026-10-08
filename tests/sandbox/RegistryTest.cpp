//
// Run the actual Parameters parser over query replies. DriverEntry cannot
// be linked into DispatchSandbox; testing copied parsing rules would miss
// malformed lengths and arithmetic wrap in the shipping implementation.
//
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "SandboxSocket.h"
#include "../../src/Registry.h"
}

namespace
{
class RegistryTest : public ::testing::Test
{
protected:
    WCHAR PortBuffer[BLORGFS_REG_PORT_MAX_CHARS] = {};
    WCHAR HostBuffer[BLORGFS_REG_HOST_MAX_CHARS] = {};
    WCHAR PathBuffer[BLORGFS_REG_DISK_CACHE_PATH_MAX_CHARS] = {};
    UNICODE_STRING Port = {0, sizeof(PortBuffer), PortBuffer};
    UNICODE_STRING Host = {0, sizeof(HostBuffer), HostBuffer};
    UNICODE_STRING Path = {0, sizeof(PathBuffer), PathBuffer};

    void SetUp() override
    {
        SandboxInitialize();
        RegistryModelReset();
        global.ReadAheadGranularity = READ_AHEAD_GRANULARITY;
        global.ReadAheadMaxGranularity = READ_AHEAD_MAX_GRANULARITY;
        global.ReadFairBudget = READ_FAIR_BUDGET;
        global.ReadAheadAdapt = TRUE;
        global.ChangeFeed = TRUE;
        global.ReadAheadSlackGrowth = TRUE;
        global.TlsEnabled = FALSE;
        global.SubtreeEntries = SUBTREE_ENTRIES;
        global.DiskCacheMb = 0;
    }

    void TearDown() override
    {
        RegistryModelReset();
        SandboxCleanup();
    }

    void Read()
    {
        UNICODE_STRING service = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\Services\\BlorgFS");
        BlorgReadRegistryConfig(&service, &Port, &Host, &Path);
    }

    void Dword(PCWSTR Name, ULONG Value, ULONG Length = sizeof(ULONG))
    {
        RegistryModelValue(Name, REG_DWORD, &Value, Length);
        Read();
    }
};
}

TEST_F(RegistryTest, MissingKeyAndValuesKeepDefaults)
{
    Read();
    RegistryModelOpenStatus(STATUS_OBJECT_NAME_NOT_FOUND);
    Read();
    EXPECT_EQ(READ_AHEAD_GRANULARITY, global.ReadAheadGranularity);
    EXPECT_EQ(0u, global.DiskCacheMb);
    EXPECT_EQ(0, Port.Length);
    EXPECT_EQ(0, Host.Length);
    EXPECT_EQ(0, Path.Length);
}

TEST_F(RegistryTest, DwordsNeedTheExactTypeAndLength)
{
    const ULONG value = 1;
    for (ULONG length : {0u, 1u, 2u, 3u})
    {
        Dword(L"DiskCacheMb", value, length);
        EXPECT_EQ(0u, global.DiskCacheMb);
    }
    RegistryModelValue(L"DiskCacheMb", REG_BINARY, &value, sizeof(value));
    Read();
    EXPECT_EQ(0u, global.DiskCacheMb);
    Dword(L"DiskCacheMb", 64);
    EXPECT_EQ(64u, global.DiskCacheMb);
}

TEST_F(RegistryTest, TruncatedAndOversizedQueryRepliesKeepDefaults)
{
    const ULONG value = 64;
    RegistryModelValue(L"DiskCacheMb", REG_DWORD, &value, sizeof(value));
    const ULONG header = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
    for (ULONG length : {header - 1, header, header + 3, MAXULONG})
    {
        RegistryModelQueryResult(STATUS_SUCCESS, length, sizeof(value));
        Read();
        EXPECT_EQ(0u, global.DiskCacheMb);
    }
    RegistryModelQueryResult(STATUS_BUFFER_TOO_SMALL, header + 4, 4);
    Read();
    EXPECT_EQ(0u, global.DiskCacheMb);
}

TEST_F(RegistryTest, GranulesRejectKilobyteWrapAndNonPowersOfTwo)
{
    const ULONG invalid[] = {1, 3, 100, 0x400000, 0x400004, MAXULONG};
    for (ULONG value : invalid)
    {
        Dword(L"ReadAheadGranularityKb", value);
        EXPECT_EQ(READ_AHEAD_GRANULARITY, global.ReadAheadGranularity);
        Dword(L"ReadAheadMaxGranularityKb", value);
        EXPECT_EQ(READ_AHEAD_MAX_GRANULARITY, global.ReadAheadMaxGranularity);
    }
    Dword(L"ReadAheadGranularityKb", 0);
    EXPECT_EQ(0u, global.ReadAheadGranularity);
    Dword(L"ReadAheadGranularityKb", 4);
    EXPECT_EQ(PAGE_SIZE, global.ReadAheadGranularity);
    Dword(L"ReadFairBudgetKb", 0x400000u);
    EXPECT_EQ(READ_FAIR_BUDGET, global.ReadFairBudget);
}

TEST_F(RegistryTest, CountedStringsAcceptAnOptionalTerminator)
{
    const WCHAR port[] = L"65535";
    for (ULONG length : {C_CAST(ULONG, sizeof(port)), C_CAST(ULONG, sizeof(port) - sizeof(WCHAR))})
    {
        RegistryModelValue(L"RemotePort", REG_SZ, port, length);
        Read();
        EXPECT_EQ(L"65535", std::wstring(Port.Buffer, Port.Length / sizeof(WCHAR)));
    }
    const WCHAR host[] = L"backend";
    RegistryModelValue(L"RemoteHost", REG_SZ, host, sizeof(host) - sizeof(WCHAR));
    Read();
    EXPECT_EQ(L"backend", std::wstring(Host.Buffer, Host.Length / sizeof(WCHAR)));
}

TEST_F(RegistryTest, StringsRejectOddLengthsEmbeddedNulsAndBadPorts)
{
    const WCHAR odd[] = L"host";
    RegistryModelValue(L"RemoteHost", REG_SZ, odd, sizeof(odd) - 1);
    Read();
    EXPECT_EQ(0, Host.Length);
    const WCHAR embedded[] = {L'a', 0, L'b', 0};
    RegistryModelValue(L"DiskCachePath", REG_SZ, embedded, sizeof(embedded));
    Read();
    EXPECT_EQ(0, Path.Length);
    for (const WCHAR* port : {L"", L"0", L"65536", L"123456", L"80x", L"-1"})
    {
        RegistryModelValue(L"RemotePort", REG_SZ, port, C_CAST(ULONG, (wcslen(port) + 1) * sizeof(WCHAR)));
        Read();
        EXPECT_EQ(0, Port.Length);
    }
}

TEST_F(RegistryTest, PathCapacityIsCountedAndTheDeclaredMaximumFits)
{
    std::wstring path(BLORGFS_REG_DISK_CACHE_PATH_MAX_CHARS, L'x');
    RegistryModelValue(L"DiskCachePath", REG_SZ, path.data(), C_CAST(ULONG, path.size() * sizeof(WCHAR)));
    Read();
    EXPECT_EQ(sizeof(PathBuffer), Path.Length);
    Path.Length = 0;
    Path.MaximumLength = 2;
    Read();
    EXPECT_EQ(0, Path.Length);
}
