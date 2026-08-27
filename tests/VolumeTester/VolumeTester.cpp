// VolumeTester.cpp : smoke checks that a mounted BlorgFS volume answers
// the standard volume-information surfaces. Exit code is 0 only when every
// probe succeeded, so it can gate a script.
//
#include <Windows.h>
#include <stdio.h>

#define MAX_NULL_TERMINATED_PATH (MAX_PATH + 1)

static int PrintVolumeInformation(
    const char* label,
    const WCHAR* volumeNameBuffer,
    DWORD serialNumber,
    DWORD maxComponentLength,
    DWORD fileSystemFlags,
    const WCHAR* fileSystemName)
{
    printf("%s: Volume Name: %ws\n", label, volumeNameBuffer);
    printf("%s: Serial Number: %lu\n", label, serialNumber);
    printf("%s: Max Component Length: %lu\n", label, maxComponentLength);
    printf("%s: File System Flags: %lu\n", label, fileSystemFlags);
    printf("%s: File System Name: %ws\n", label, fileSystemName);

    //
    // A volume whose filesystem name is empty or not ours is answering, but
    // not with anything this driver wrote -- worth failing on rather than
    // printing and passing.
    //
    if (0 == fileSystemName[0])
    {
        printf("%s: FAILED -- empty filesystem name\n", label);
        return 1;
    }

    return 0;
}

static int OpenVolume(void)
{
	HANDLE hVolume = CreateFile(
	L"\\\\.\\B:",
	GENERIC_READ,
	FILE_SHARE_READ,
	NULL,
	OPEN_EXISTING,
	0,
	NULL
	);

	if (hVolume == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		printf("OpenVolume: Failed to open volume. Error code: %lu\n", error);

		// Common error codes:
		if (error == ERROR_ACCESS_DENIED)
		{
			printf("OpenVolume: Admin privileges required!\n");
		}
		else if (error == ERROR_FILE_NOT_FOUND)
		{
			printf("OpenVolume: Drive B: does not exist!\n");
		}
		return 1;
	}

	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };
	WCHAR volumeName[MAX_NULL_TERMINATED_PATH] = { 0 };
	DWORD serialNumber;

	BOOL ok = GetVolumeInformationByHandleW(hVolume, volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	CloseHandle(hVolume);

	if (!ok)
	{
		printf("OpenVolume: Failed to get volume information\n");
		return 1;
	}

	return PrintVolumeInformation("OpenVolume", volumeName, serialNumber, maxComponentLength, fileSystemFlags, fileSystemName);
}

static int OpenRootDirectory(void)
{
	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };
	WCHAR volumeName[MAX_NULL_TERMINATED_PATH] = { 0 };
	DWORD serialNumber;

	BOOL ok = GetVolumeInformationW(L"B:\\", volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	if (!ok)
	{
		printf("OpenRootDirectory: Failed to get volume information\n");
		return 1;
	}

	return PrintVolumeInformation("OpenRootDirectory", volumeName, serialNumber, maxComponentLength, fileSystemFlags, fileSystemName);
}

static int OpenMainRootDirectory(void)
{
	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };
	WCHAR volumeName[MAX_NULL_TERMINATED_PATH] = { 0 };
	DWORD serialNumber;

	BOOL ok = GetVolumeInformationW(L"C:\\", volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	if (!ok)
	{
		printf("OpenMainRootDirectory: Failed to get volume information\n");
		return 1;
	}

	return PrintVolumeInformation("OpenMainRootDirectory", volumeName, serialNumber, maxComponentLength, fileSystemFlags, fileSystemName);
}


int main()
{
	int failed = 0;

	failed |= OpenVolume();
	failed |= OpenRootDirectory();
	failed |= OpenMainRootDirectory();

	return failed;
}
