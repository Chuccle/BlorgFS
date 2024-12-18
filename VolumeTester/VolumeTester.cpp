// VolumeTester.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <Windows.h>
#include <stdio.h>

#define MAX_NULL_TERMINATED_PATH MAX_PATH + 1

int main()
{
	HANDLE hVolume = CreateFile(
		L"\\\\.\\B:",
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		0,
		NULL
	);

	if (hVolume == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		printf("Failed to open volume. Error code: %d\n", error);

		// Common error codes:
		if (error == ERROR_ACCESS_DENIED)
		{
			printf("Admin privileges required!\n");
		}
		else if (error == ERROR_FILE_NOT_FOUND)
		{
			printf("Drive B: does not exist!\n");
		}
		return 1;
	}

	DWORD maxComponentLength;
	DWORD fileSystemFlags;
	WCHAR fileSystemName[MAX_NULL_TERMINATED_PATH] = { 0 };

	BOOL boobs = GetVolumeInformationByHandleW(hVolume, NULL, 0, NULL, &maxComponentLength, &fileSystemFlags, fileSystemName, MAX_NULL_TERMINATED_PATH);

	if (boobs)
	{
		printf("Max Component Length: %d\n", maxComponentLength);
		printf("File System Flags: %d\n", fileSystemFlags);
		printf("File System Name: %ws\n", fileSystemName);
	}
	else
	{
		printf("Failed to get volume information\n");
		return 1;
	}

	return 0;
}

