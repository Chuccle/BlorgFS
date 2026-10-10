#pragma once

//
// One place that builds a synthetic DIRECTORY_INFO the way DirCtrlComplete
// would have cached one, for the tests that need a warm directory without a
// network round trip (CreateDirectoryTest.cpp's listing-hit branch,
// DirCtrlTest.cpp's enumeration).
//
// Shared rather than copied per fixture because this encodes the listing's
// layout: the block comes from BlorgAllocateDirectoryInfo, the names are
// laid out from NamesOffset on, and BlorgIndexDirectoryInfo chains them, as
// Client.c's HttpDecodeListing does. Two hand-maintained copies of that is
// exactly the drift this avoids: a layout change would fix one caller and
// quietly leave the other building a structure the driver reads
// differently. The entries themselves are filled through the real
// BlorgGetFileEntry/BlorgGetSubDirEntry accessors for the same reason.
//

#include "..\..\src\Driver.h"

#include <cwchar>
#include <string>
#include <vector>

//
// Files and SubDirs under the names given, files sized 1000+N. The result
// carries one reference, as a deserialized listing does; the caller drops it
// with BlorgReleaseDirectoryInfo once whatever it published into holds its own.
//
inline PDIRECTORY_INFO BuildListing(const std::vector<std::wstring>& Files, const std::vector<std::wstring>& SubDirs)
{
    SIZE_T nameBytes = 0;

    for (const auto& name : Files)
    {
        nameBytes += (name.size() + 1) * sizeof(WCHAR);
    }

    for (const auto& name : SubDirs)
    {
        nameBytes += (name.size() + 1) * sizeof(WCHAR);
    }

    PDIRECTORY_INFO info = BlorgAllocateDirectoryInfo(Files.size(), SubDirs.size(), nameBytes);

    if (!info)
    {
        return nullptr;
    }

    PWCH cursor = C_CAST(PWCH, C_CAST(PUCHAR, info) + info->NamesOffset);

    for (SIZE_T i = 0; i < Files.size(); ++i)
    {
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(info, i);

        file->Size = 1000 + i;
        file->Name = cursor;
        file->NameLength = Files[i].size();
        wmemcpy(cursor, Files[i].c_str(), Files[i].size() + 1);
        cursor += Files[i].size() + 1;
    }

    for (SIZE_T i = 0; i < SubDirs.size(); ++i)
    {
        PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(info, i);

        sub->Name = cursor;
        sub->NameLength = SubDirs[i].size();
        wmemcpy(cursor, SubDirs[i].c_str(), SubDirs[i].size() + 1);
        cursor += SubDirs[i].size() + 1;
    }

    BlorgIndexDirectoryInfo(info);

    return info;
}

//
// Counted entries named "file<N>.bin" and "dir<N>". Pad lengthens every
// name to that many characters with leading 'x's, for the tests that need a
// listing of a given size in bytes rather than in entries: at 250 an entry
// takes about 570 bytes.
//
inline PDIRECTORY_INFO BuildSyntheticListing(int FileCount, int SubDirCount, size_t Pad = 0)
{
    std::vector<std::wstring> files;
    std::vector<std::wstring> subDirs;

    for (int i = 0; i < FileCount; ++i)
    {
        std::wstring name = L"file" + std::to_wstring(i) + L".bin";
        files.push_back((name.size() < Pad) ? std::wstring(Pad - name.size(), L'x') + name : name);
    }

    for (int i = 0; i < SubDirCount; ++i)
    {
        std::wstring name = L"dir" + std::to_wstring(i);
        subDirs.push_back((name.size() < Pad) ? std::wstring(Pad - name.size(), L'x') + name : name);
    }

    return BuildListing(files, subDirs);
}

//
// One file and one subdirectory under caller-chosen names, for tests that
// assert on specific names rather than on counts.
//
inline PDIRECTORY_INFO BuildSyntheticListingNamed(const wchar_t* FileName, const wchar_t* SubDirName)
{
    PDIRECTORY_INFO info = BuildListing({ FileName }, { SubDirName });

    if (!info)
    {
        return nullptr;
    }

    BlorgGetFileEntry(info, 0)->Size = 2048;

    return info;
}
