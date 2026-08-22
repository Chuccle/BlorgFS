// VolumeTester.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <Windows.h>
#include <stdio.h>

#define MAX_NULL_TERMINATED_PATH MAX_PATH + 1

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
		printf("OpenVolume: Failed to open volume. Error code: %d\n", error);

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

	BOOL boobs = GetVolumeInformationByHandleW(hVolume, volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	CloseHandle(hVolume);

	if (boobs)
	{
		printf("OpenRootDirectory: Volume Name: %ws\n", volumeName);
		printf("OpenRootDirectory: Serial Number: %d\n", serialNumber);
		printf("OpenRootDirectory: Max Component Length: %d\n", maxComponentLength);
		printf("OpenRootDirectory: File System Flags: %d\n", fileSystemFlags);
		printf("OpenRootDirectory: File System Name: %ws\n", fileSystemName);
	}
	else
	{
		printf("OpenRootDirectory: Failed to get volume information\n");
		return 1;
	}

	return 0;
}

static int OpenRootDirectory(void)
{

	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };
	WCHAR volumeName[MAX_NULL_TERMINATED_PATH] = { 0 };
	DWORD serialNumber;

	BOOL boobs = GetVolumeInformationW(L"B:\\", volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	if (boobs)
	{
		printf("OpenRootDirectory: Volume Name: %ws\n", volumeName);
		printf("OpenRootDirectory: Serial Number: %d\n", serialNumber);
		printf("OpenRootDirectory: Max Component Length: %d\n", maxComponentLength);
		printf("OpenRootDirectory: File System Flags: %d\n", fileSystemFlags);
		printf("OpenRootDirectory: File System Name: %ws\n", fileSystemName);
	}
	else
	{
		printf("OpenRootDirectory: Failed to get volume information\n");
		return 1;
	}

	return 0;
}

static int OpenMainRootDirectory(void)
{
	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };
	WCHAR volumeName[MAX_NULL_TERMINATED_PATH] = { 0 };
	DWORD serialNumber;

	BOOL boobs = GetVolumeInformationW(L"C:\\", volumeName, MAX_NULL_TERMINATED_PATH, &serialNumber, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	if (boobs)
	{
		printf("OpenMainRootDirectory: Volume Name: %ws\n", volumeName);
		printf("OpenMainRootDirectory: Serial Number: %d\n", serialNumber);
		printf("OpenMainRootDirectory: Max Component Length: %d\n", maxComponentLength);
		printf("OpenMainRootDirectory: File System Flags: %d\n", fileSystemFlags);
		printf("OpenMainRootDirectory: File System Name: %ws\n", fileSystemName);
	}
	else
	{
		printf("OpenMainRootDirectory: Failed to get volume information\n");
		return 1;
	}

	return 0;
}


int main()
{
	int returnCode = 0;
	
	returnCode = OpenVolume();
	if (returnCode != 0)
	{
		printf("OpenVolume failed\n");
	}

	returnCode = OpenRootDirectory();
	if (returnCode != 0)
	{
		printf("OpenRootDirectory failed\n");
	}

	returnCode = OpenMainRootDirectory();
	if (returnCode != 0)
	{
		printf("OpenMainRootDirectory failed\n");
	}

	return 0;
}

