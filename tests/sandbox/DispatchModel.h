#pragma once

//
// Prototypes for everything DispatchModel.c implements.
//
// This header is included last, after NtShim, FsRtlShim, IoShim and
// FileInfoShim, because these signatures draw types from all of them --
// PCC_FILE_SIZES, PSHARE_ACCESS, PETHREAD, PIO_STATUS_BLOCK. Declaring
// them earlier is what produced "syntax error: identifier
// 'PCC_FILE_SIZES'".
//
// They matter beyond tidiness: without a prototype MSVC assumes "extern
// int", so a function returning a pointer or a 64-bit value has its result
// silently truncated to 32 bits, and the dispatch file calling it gets a
// wrong answer rather than a build error.
//

#ifdef __cplusplus
extern "C" {
#endif

//
// Implemented by DispatchModel.c. Declared here so the dispatch files get
// real prototypes: without them MSVC assumes "extern int", and a function
// returning a pointer or a 64-bit value silently truncates.
//
VOID KdBreakPoint(VOID);

//
// Controls for the two branches DispatchModel.c's default stubs made
// structurally unreachable: the oplock-pending path (Create.c x3, Read.c)
// and the cache-miss repost path (Read.c / FspWorkQueue.c). See
// DispatchModel.c for why each mattered. The FSP worker's shutdown wait
// used to need a third of these (ShimSignalWaitObject1) before
// KeWaitForMultipleObjects became a real wait on real events -- see
// FspWorkQueueStressTest.cpp.
//
VOID ShimForceNextOplockCheck(NTSTATUS Status);
VOID ShimForceNextCcCopyReadMiss(VOID);

BOOLEAN ExIsResourceAcquiredExclusiveLite(PERESOURCE Resource);
VOID ExConvertExclusiveToSharedLite(PERESOURCE Resource);

VOID IoAcquireVpbSpinLock(PKIRQL Irql);
VOID IoReleaseVpbSpinLock(KIRQL Irql);

VOID ObReferenceObject(PVOID Object);
VOID ObDereferenceObject(PVOID Object);

PVOID KeGetCurrentThread(VOID);
LONG KeSetBasePriorityThread(PVOID Thread, LONG Priority);
ULONG KeGetCurrentProcessorIndex(VOID);
ULONG KeQueryActiveProcessorCountEx(USHORT Group);
ULONG KeQueryMaximumProcessorCountEx(USHORT Group);
ULONG64 KeQueryInterruptTime(VOID);
VOID ShimAdvanceInterruptTime(ULONG64 Ticks100ns);

VOID ProbeForRead(PVOID Address, SIZE_T Length, ULONG Alignment);

NTSTATUS KeWaitForMultipleObjects(
    ULONG Count, PVOID Object[], WAIT_TYPE WaitType, KWAIT_REASON WaitReason,
    KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlockArray);

NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG Access, PVOID ObjectAttributes,
    HANDLE ProcessHandle, PVOID ClientId, PVOID StartRoutine, PVOID StartContext);
NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);

NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK Access, POBJECT_TYPE Type,
    KPROCESSOR_MODE Mode, PVOID* Object, PVOID HandleInformation);
NTSTATUS ZwClose(HANDLE Handle);

NTSTATUS CreateBlorgVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject);

NTSTATUS RtlCreateSecurityDescriptor(PVOID Descriptor, ULONG Revision);
NTSTATUS RtlSetDaclSecurityDescriptor(PVOID Descriptor, BOOLEAN DaclPresent, PVOID Dacl, BOOLEAN Defaulted);
NTSTATUS RtlSetOwnerSecurityDescriptor(PVOID Descriptor, PSID Owner, BOOLEAN Defaulted);
NTSTATUS RtlSetGroupSecurityDescriptor(PVOID Descriptor, PSID Group, BOOLEAN Defaulted);
NTSTATUS RtlAbsoluteToSelfRelativeSD(PVOID Absolute, PVOID SelfRelative, PULONG Length);
NTSTATUS SeQuerySecurityDescriptorInfo(PULONG Information, PVOID Descriptor, PULONG Length, PVOID* ObjectsSecurityDescriptor);

VOID RtlInitUnicodeString(PUNICODE_STRING Destination, PCWSTR Source);
BOOLEAN RtlPrefixUnicodeString(PCUNICODE_STRING Prefix, PCUNICODE_STRING String, BOOLEAN IgnoreCase);
NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING Destination, PCUNICODE_STRING Source, BOOLEAN Allocate);

NTSTATUS FsRtlCheckOplock(POPLOCK Oplock, PIRP Irp, PVOID Context, PVOID Waiter, PVOID Prepost);
BOOLEAN FsRtlOplockIsFastIoPossible(POPLOCK Oplock);
BOOLEAN FsRtlOplockIsSharedRequest(PIRP Irp);
NTSTATUS FsRtlOplockBreakH(POPLOCK Oplock, PIRP Irp, ULONG Flags, PVOID Context, PVOID Callback, PVOID Prepost);
BOOLEAN FsRtlFastUnlockAll(PFILE_LOCK FileLock, PFILE_OBJECT FileObject, PEPROCESS Process, PVOID Context);
BOOLEAN FsRtlCheckLockForReadAccess(PFILE_LOCK FileLock, PIRP Irp);
VOID FsRtlNotifyCleanup(PNOTIFY_SYNC Sync, PLIST_ENTRY List, PVOID Context);
VOID FsRtlNotifyFullChangeDirectory(
    PNOTIFY_SYNC Sync, PLIST_ENTRY List, PVOID Context, PSTRING FullName,
    BOOLEAN WatchTree, BOOLEAN IgnoreBuffer, ULONG Filter, PIRP Irp,
    PVOID TraverseCallback, PVOID SubjectContext);
BOOLEAN FsRtlIsNameInExpression(PUNICODE_STRING Expression, PUNICODE_STRING Name, BOOLEAN IgnoreCase, PWCH Upcase);
BOOLEAN FsRtlAreNamesEqual(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN IgnoreCase, PCWCH Upcase);

VOID CcInitializeCacheMap(PFILE_OBJECT F, PCC_FILE_SIZES Sizes, BOOLEAN PinAccess, PCACHE_MANAGER_CALLBACKS Callbacks, PVOID Context);
BOOLEAN CcUninitializeCacheMap(PFILE_OBJECT F, PLARGE_INTEGER TruncateSize, PVOID Event);
VOID CcSetReadAheadGranularity(PFILE_OBJECT F, ULONG Granularity);
BOOLEAN CcCopyReadEx(PFILE_OBJECT F, PLARGE_INTEGER Offset, ULONG Length, BOOLEAN Wait, PVOID Buffer, PIO_STATUS_BLOCK Status, PETHREAD Thread);
VOID CcMdlRead(PFILE_OBJECT F, PLARGE_INTEGER Offset, ULONG Length, PMDL* Mdl, PIO_STATUS_BLOCK Status);
VOID CcFlushCache(PVOID SectionPointer, PLARGE_INTEGER Offset, ULONG Length, PIO_STATUS_BLOCK Status);

VOID IoSetShareAccess(ACCESS_MASK Desired, ULONG Share, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess);
NTSTATUS IoCheckShareAccess(ACCESS_MASK Desired, ULONG Share, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess, BOOLEAN Update);
VOID IoRemoveShareAccess(PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess);
BOOLEAN IoIsOperationSynchronous(PIRP Irp);
PEPROCESS IoGetRequestorProcess(PIRP Irp);


#ifdef __cplusplus
}
#endif
