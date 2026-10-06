#pragma once

//
// The cache file DiskCache.c keeps its blocks in, and the volume under it.
//
// DiskCache.c opens a file on another file system with ZwCreateFile, sizes
// it, and from then on sends that file system its own non-cached read and
// write IRPs. DiskCacheModel.c stands in for that file system: the file is
// a buffer, and IoCallDriver on its device copies between the buffer and
// the IRP's MDL and runs the completion routine at DISPATCH_LEVEL, as a
// disk completion would. A test can fail the next read or write, or hold
// the completions back to run them when it chooses.
//
// The security calls DiskCache.c makes before trusting the file are the
// real Win32 ones underneath (InitializeAcl, EqualSid and the rest), and
// the file's owner is what ZwQuerySecurityObject reports, which a test can
// set to someone other than SYSTEM or Administrators.
//
// Included after DispatchModel.h, for PIO_STATUS_BLOCK,
// FILE_INFORMATION_CLASS and OBJECT_ATTRIBUTES.
//

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FILE_NO_INTERMEDIATE_BUFFERING
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#endif
#ifndef FILE_RANDOM_ACCESS
#define FILE_RANDOM_ACCESS              0x00000800
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT         0x00200000
#endif

#define OBJ_CASE_INSENSITIVE 0x00000040L
#define OBJ_DONT_REPARSE     0x00001000L

typedef struct _FILE_END_OF_FILE_INFORMATION
{
    LARGE_INTEGER EndOfFile;
} FILE_END_OF_FILE_INFORMATION, * PFILE_END_OF_FILE_INFORMATION;

NTSTATUS ZwCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS ZwSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);
NTSTATUS ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS ZwQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded);
NTSTATUS RtlGetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, PSID* Owner, PBOOLEAN OwnerDefaulted);
BOOLEAN RtlEqualSid(PSID Sid1, PSID Sid2);
NTSTATUS RtlCreateAcl(PACL Acl, ULONG AclLength, ULONG AclRevision);
NTSTATUS RtlAddAccessAllowedAce(PACL Acl, ULONG AceRevision, ACCESS_MASK AccessMask, PSID Sid);
PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT FileObject);

//
// Empties the volume: no file, owned by Administrators when created,
// completions inline, nothing failing. Frees what the last file held.
//
VOID DiskCacheModelReset(VOID);

// The volume the cache file is on, for a test that needs it named.
PDEVICE_OBJECT DiskCacheModelDevice(VOID);

// Whom ZwQuerySecurityObject names as the owner of what ZwCreateFile opens.
VOID DiskCacheModelSetOwner(PSID Owner);

// Fails the next IRP_MJ_READ or IRP_MJ_WRITE sent to the file with Status.
VOID DiskCacheModelFailNext(UCHAR MajorFunction, NTSTATUS Status);

//
// While Hold is set, IRPs sent to the file are queued rather than
// completed; DiskCacheModelCompleteHeld completes them in the order they
// were sent and returns how many it completed.
//
VOID DiskCacheModelHold(BOOLEAN Hold);
ULONG DiskCacheModelCompleteHeld(VOID);

// IRPs of MajorFunction the file has been sent since the last reset.
ULONG DiskCacheModelIrps(UCHAR MajorFunction);

#ifdef __cplusplus
}
#endif
