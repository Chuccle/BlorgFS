#pragma once

//
// One place that builds a synthetic DIRECTORY_INFO the way BlorgDirComplete
// would have cached one, for the tests that need a warm directory without a
// network round trip (CreateDirectoryTest.cpp's listing-hit branch,
// DirCtrlTest.cpp's enumeration).
//
// Shared rather than copied per fixture because this encodes the listing's
// wire layout -- the FilesOffset/SubDirsOffset arithmetic that must agree
// with what Client.c's HttpDeserializeDirectoryInfo produces and what
// BlorgGetFileEntry/BlorgGetSubDirEntry read back. Two hand-maintained copies of that
// arithmetic is exactly the drift this avoids: a layout change would fix one
// caller and quietly leave the other building a structure the driver reads
// differently. The entries themselves are filled through the real
// BlorgGetFileEntry/BlorgGetSubDirEntry accessors for the same reason.
//

#include "..\..\src\Driver.h"

#include <string>

//
// Counted entries named "file<N>.bin" and "dir<N>", sized 1000+N. Caller
// owns the result and frees it with BlorgFreeHttpDirectoryInfo (or lets the DCB
// that adopts it do so).
//
inline PDIRECTORY_INFO BuildSyntheticListing(int FileCount, int SubDirCount)
{
    const SIZE_T size = sizeof(DIRECTORY_INFO) +
        C_CAST(SIZE_T, FileCount) * sizeof(DIRECTORY_FILE_METADATA) +
        C_CAST(SIZE_T, SubDirCount) * sizeof(DIRECTORY_SUBDIR_METADATA);

    PDIRECTORY_INFO info = C_CAST(PDIRECTORY_INFO, ExAllocatePoolZero(PagedPool, size, 'TCRT'));

    if (!info)
    {
        return nullptr;
    }

    info->FilesOffset = sizeof(DIRECTORY_INFO);
    info->SubDirsOffset = C_CAST(ULONG, sizeof(DIRECTORY_INFO) +
        C_CAST(SIZE_T, FileCount) * sizeof(DIRECTORY_FILE_METADATA));
    info->FileCount = FileCount;
    info->SubDirCount = SubDirCount;

    for (int i = 0; i < FileCount; ++i)
    {
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(info, i);
        std::wstring name = L"file" + std::to_wstring(i) + L".bin";

        file->Size = 1000 + i;
        file->NameLength = name.size();
        wcscpy_s(file->Name, MAX_NAME_LEN, name.c_str());
    }

    for (int i = 0; i < SubDirCount; ++i)
    {
        PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(info, i);
        std::wstring name = L"dir" + std::to_wstring(i);

        sub->NameLength = name.size();
        wcscpy_s(sub->Name, MAX_NAME_LEN, name.c_str());
    }

    return info;
}

//
// One file and one subdirectory under caller-chosen names, for tests that
// assert on specific names rather than on counts.
//
inline PDIRECTORY_INFO BuildSyntheticListingNamed(const wchar_t* FileName, const wchar_t* SubDirName)
{
    PDIRECTORY_INFO info = BuildSyntheticListing(1, 1);

    if (!info)
    {
        return nullptr;
    }

    PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(info, 0);
    file->Size = 2048;
    file->NameLength = wcslen(FileName);
    wcscpy_s(file->Name, MAX_NAME_LEN, FileName);

    PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(info, 0);
    sub->NameLength = wcslen(SubDirName);
    wcscpy_s(sub->Name, MAX_NAME_LEN, SubDirName);

    return info;
}
