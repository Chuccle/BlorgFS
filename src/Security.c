#include "Driver.h"

//
//  Security descriptors: what each node resolves to, the access check every
//  open makes against it, and IRP_MJ_QUERY_SECURITY / IRP_MJ_SET_SECURITY.
//
//  The server stores self-relative descriptors without interpreting them
//  and hands back, with every entry, either the entry's own or the nearest
//  one stored above it and how far up that is (metadata_flatbuffer.fbs).
//  What an inheriting entry gets is worked out here, the way NTFS works it
//  out when a file is created: SeAssignSecurityEx with the parent's
//  descriptor. An entry two or more levels below the descriptor it
//  inherits from takes it through one directory first, since what a
//  directory passes on to its children can differ from what it holds
//  itself (an ACE marked no-propagate reaches only the first level).
//
//  Every descriptor is interned once, in a table that lives as long as the
//  driver, and named everywhere else by its index: a listing entry, a path
//  cache entry and a node each carry a ULONG rather than a copy, and what
//  is inherited from each descriptor is computed once and remembered on
//  it. A tree with no stored descriptor at all costs nothing: its entries
//  resolve to BLORGFS_SECURITY_DEFAULT_FILE or _DIRECTORY, built at load.
//
//  The table is bounded in entries and in bytes. A descriptor that does not
//  fit, or that is not a valid one, resolves to BLORGFS_SECURITY_LOCKED,
//  which grants SYSTEM and Administrators only: refusing an open the server
//  would have allowed is recoverable, granting one it would have refused is
//  not.
//
//  Ids are handed out in order and an entry is published to the id array
//  before its id is returned, so looking one up needs no lock. Interning
//  takes a push lock, and runs only where a response is decoded or a
//  descriptor is inherited, both at PASSIVE_LEVEL.
//

#define SECURITY_TAG 'CESB'

//
//  Bounds on the interned table. Real trees hold a handful of distinct
//  descriptors; these exist so a hostile server cannot grow the table
//  without limit.
//
#define SECURITY_MAX_ENTRIES 16384u
#define SECURITY_MAX_BYTES (16u * 1024u * 1024u)
#define SECURITY_MAX_DESCRIPTOR_BYTES (64u * 1024u)
#define SECURITY_BUCKETS 1024u

//
//  One interned descriptor. Inherited[deep][directory] is the id plus one
//  of what an entry inherits from this descriptor: deep when it is two or
//  more levels below it, directory when the entry is one. Zero until first
//  asked for; two threads computing it at once arrive at the same answer.
//
typedef struct _SECURITY_ENTRY
{
    struct _SECURITY_ENTRY* Next; // Hash chain, under SecurityLock
    ULONG64 Hash;                 // Of the descriptor's bytes
    ULONG   Length;               // Of Descriptor, in bytes
    ULONG   Id;                   // Index in SecurityById
    LONG    Inherited[2][2];      // Memoized inheritance; see above
    UCHAR   Descriptor[8];        // Self-relative; Length bytes from here
} SECURITY_ENTRY, * PSECURITY_ENTRY;

CHECK_PADDING_BETWEEN(SECURITY_ENTRY, Next, Hash);
CHECK_PADDING_BETWEEN(SECURITY_ENTRY, Hash, Length);
CHECK_PADDING_BETWEEN(SECURITY_ENTRY, Length, Id);
CHECK_PADDING_BETWEEN(SECURITY_ENTRY, Id, Inherited);
CHECK_PADDING_BETWEEN(SECURITY_ENTRY, Inherited, Descriptor);
CHECK_PADDING_END(SECURITY_ENTRY, Descriptor);

static PSECURITY_ENTRY* SecurityById;
static PSECURITY_ENTRY SecurityBuckets[SECURITY_BUCKETS];
static EX_PUSH_LOCK SecurityLock;
static ULONG SecurityCount;
static SIZE_T SecurityBytes;

//
//  FNV-1a over the descriptor's bytes.
//
static ULONG64 SecurityHash(const UCHAR* Bytes, SIZE_T Length)
{
    ULONG64 hash = 0xcbf29ce484222325ull;

    for (SIZE_T i = 0; i < Length; i++)
    {
        hash = (hash ^ Bytes[i]) * 0x100000001b3ull;
    }

    return hash;
}

//
//  The entry an id names, or NULL for an id never handed out (including
//  BLORGFS_SECURITY_UNKNOWN) or before the table exists.
//
static PSECURITY_ENTRY SecurityEntry(ULONG Id)
{
    if (!SecurityById || (Id >= SECURITY_MAX_ENTRIES))
    {
        return NULL;
    }

    return ReadPointerAcquire(C_CAST(PVOID*, &SecurityById[Id]));
}

//
//  The descriptor an id names. Anything not in the table is answered as
//  BLORGFS_SECURITY_LOCKED, which is there from load until unload.
//
static PSECURITY_DESCRIPTOR SecurityDescriptor(ULONG Id)
{
    PSECURITY_ENTRY entry = SecurityEntry(Id);

    if (!entry)
    {
        entry = SecurityEntry(BLORGFS_SECURITY_LOCKED);
    }

    return entry ? C_CAST(PSECURITY_DESCRIPTOR, entry->Descriptor) : NULL;
}

//
//  Searches one hash chain for an equal descriptor. Caller holds
//  SecurityLock, shared or exclusive.
//
static PSECURITY_ENTRY SecurityFind(ULONG64 Hash, const UCHAR* Bytes, SIZE_T Length)
{
    for (PSECURITY_ENTRY entry = SecurityBuckets[Hash % SECURITY_BUCKETS]; entry; entry = entry->Next)
    {
        if ((entry->Hash == Hash) && (entry->Length == Length) && RtlEqualMemory(entry->Descriptor, Bytes, Length))
        {
            return entry;
        }
    }

    return NULL;
}

//
//  Returns the id of Descriptor, a self-relative descriptor of Length
//  bytes, interning it on first sight. A descriptor that is not valid, or
//  that the table has no room for, is BLORGFS_SECURITY_LOCKED. PASSIVE_LEVEL.
//
//  The copy is allocated before the exclusive acquire, so the lock is never
//  held across an allocation; a racing intern of the same bytes means it is
//  freed again.
//
ULONG BlorgSecurityIntern(const VOID* Descriptor, SIZE_T Length)
{
    if (!SecurityById || (0 == Length) || (Length > SECURITY_MAX_DESCRIPTOR_BYTES) ||
        !RtlValidRelativeSecurityDescriptor(C_CAST(PVOID, Descriptor), C_CAST(ULONG, Length), 0))
    {
        return BLORGFS_SECURITY_LOCKED;
    }

    const UCHAR* bytes = Descriptor;
    const ULONG64 hash = SecurityHash(bytes, Length);

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&SecurityLock);

    PSECURITY_ENTRY found = SecurityFind(hash, bytes, Length);

    ExReleasePushLockShared(&SecurityLock);
    KeLeaveCriticalRegion();

    if (found)
    {
        return found->Id;
    }

    PSECURITY_ENTRY entry = ExAllocatePoolZero(PagedPool, FIELD_OFFSET(SECURITY_ENTRY, Descriptor) + Length, SECURITY_TAG);

    if (!entry)
    {
        return BLORGFS_SECURITY_LOCKED;
    }

    RtlCopyMemory(entry->Descriptor, bytes, Length);
    entry->Hash = hash;
    entry->Length = C_CAST(ULONG, Length);

    ULONG id = BLORGFS_SECURITY_LOCKED;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&SecurityLock);

    found = SecurityFind(hash, bytes, Length);

    if (found)
    {
        id = found->Id;
    }
    else if ((SecurityCount < SECURITY_MAX_ENTRIES) && (SecurityBytes + Length <= SECURITY_MAX_BYTES))
    {
        id = SecurityCount;
        entry->Id = id;
        entry->Next = SecurityBuckets[hash % SECURITY_BUCKETS];
        SecurityBuckets[hash % SECURITY_BUCKETS] = entry;
        (VOID)InterlockedExchangePointer(C_CAST(PVOID*, &SecurityById[id]), entry);
        SecurityCount++;
        SecurityBytes += Length;
        entry = NULL;
    }

    ExReleasePushLockExclusive(&SecurityLock);
    KeLeaveCriticalRegion();

    if (entry)
    {
        ExFreePool(entry);
    }

    return id;
}

//
//  What an object created under Parent gets, interned: SeAssignSecurityEx
//  with no explicit descriptor, owner and group taken from the parent, and
//  no privilege or owner check, since nothing is being created on anyone's
//  behalf -- the server already holds the entry, and this only works out
//  what NTFS would have stored with it.
//
static ULONG SecurityAssign(PSECURITY_DESCRIPTOR Parent, BOOLEAN IsDirectory)
{
    SECURITY_SUBJECT_CONTEXT subject;
    SeCaptureSubjectContext(&subject);

    PSECURITY_DESCRIPTOR created = NULL;

    NTSTATUS status = SeAssignSecurityEx(
        Parent,
        NULL,
        &created,
        NULL,
        IsDirectory,
        SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT | SEF_DEFAULT_OWNER_FROM_PARENT |
            SEF_DEFAULT_GROUP_FROM_PARENT | SEF_AVOID_OWNER_CHECK | SEF_AVOID_PRIVILEGE_CHECK,
        &subject,
        IoGetFileObjectGenericMapping(),
        PagedPool);

    SeReleaseSubjectContext(&subject);

    if (!NT_SUCCESS(status))
    {
        return BLORGFS_SECURITY_LOCKED;
    }

    const ULONG id = BlorgSecurityIntern(created, RtlLengthSecurityDescriptor(created));

    (VOID)SeDeassignSecurity(&created);

    return id;
}

//
//  The id of what an entry Depth levels below Source inherits from it;
//  Depth 0 is Source itself. Remembered on Source's entry, so after the
//  first entry of each kind this is a table lookup. The deep case recurses
//  exactly once, through the depth-one directory case.
//
ULONG BlorgSecurityInherit(ULONG Source, ULONG Depth, BOOLEAN IsDirectory)
{
    if (0 == Depth)
    {
        return Source;
    }

    PSECURITY_ENTRY entry = SecurityEntry(Source);

    if (!entry)
    {
        return BLORGFS_SECURITY_LOCKED;
    }

    const ULONG deep = (1 < Depth) ? 1 : 0;
    const ULONG directory = IsDirectory ? 1 : 0;
    const LONG known = ReadAcquire(&entry->Inherited[deep][directory]);

    if (0 != known)
    {
        return C_CAST(ULONG, known - 1);
    }

    const ULONG id = deep ?
        BlorgSecurityInherit(BlorgSecurityInherit(Source, 1, TRUE), 1, IsDirectory) :
        SecurityAssign(C_CAST(PSECURITY_DESCRIPTOR, entry->Descriptor), IsDirectory);

    (VOID)InterlockedExchange(&entry->Inherited[deep][directory], C_CAST(LONG, id + 1));

    return id;
}

//
//  One access-allowed ACE of a descriptor the driver builds itself.
//
typedef struct _SECURITY_ACE_SPEC
{
    PSID        Sid;   // Who it grants to
    ACCESS_MASK Mask;  // What it grants
    ULONG       Flags; // Its inheritance flags
} SECURITY_ACE_SPEC;

CHECK_PADDING_BETWEEN(SECURITY_ACE_SPEC, Sid, Mask);
CHECK_PADDING_BETWEEN(SECURITY_ACE_SPEC, Mask, Flags);
CHECK_PADDING_END(SECURITY_ACE_SPEC, Flags);

//
//  Interns a descriptor granting what AceCount entries of Aces describe,
//  owned by Administrators with SYSTEM as its group, and returns its id.
//
static NTSTATUS SecurityBuild(const SECURITY_ACE_SPEC* Aces, ULONG AceCount, PULONG Id)
{
    ULONG aclLength = C_CAST(ULONG, sizeof(ACL));

    for (ULONG i = 0; i < AceCount; i++)
    {
        aclLength += C_CAST(ULONG, sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) + RtlLengthSid(Aces[i].Sid);
    }

    PACL acl = ExAllocatePoolZero(PagedPool, aclLength, SECURITY_TAG);

    if (!acl)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTSTATUS status = RtlCreateAcl(acl, aclLength, ACL_REVISION);

    for (ULONG i = 0; NT_SUCCESS(status) && (i < AceCount); i++)
    {
        status = RtlAddAccessAllowedAceEx(acl, ACL_REVISION, Aces[i].Flags, Aces[i].Mask, Aces[i].Sid);
    }

    SECURITY_DESCRIPTOR absolute;

    if (NT_SUCCESS(status))
    {
        status = RtlCreateSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlSetDaclSecurityDescriptor(&absolute, TRUE, acl, FALSE);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlSetOwnerSecurityDescriptor(&absolute, SeExports->SeAliasAdminsSid, FALSE);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlSetGroupSecurityDescriptor(&absolute, SeExports->SeLocalSystemSid, FALSE);
    }

    ULONG length = 0;

    if (NT_SUCCESS(status))
    {
        status = RtlAbsoluteToSelfRelativeSD(&absolute, NULL, &length);
        status = (STATUS_BUFFER_TOO_SMALL == status) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    PSECURITY_DESCRIPTOR selfRelative = NT_SUCCESS(status) ? ExAllocatePoolZero(PagedPool, length, SECURITY_TAG) : NULL;

    if (NT_SUCCESS(status) && !selfRelative)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(status))
    {
        status = RtlAbsoluteToSelfRelativeSD(&absolute, selfRelative, &length);
    }

    if (NT_SUCCESS(status))
    {
        *Id = BlorgSecurityIntern(selfRelative, length);
    }

    if (selfRelative)
    {
        ExFreePool(selfRelative);
    }

    ExFreePool(acl);

    return status;
}

//
//  Builds the table and the four descriptors every id below
//  BLORGFS_SECURITY_DEFAULT_DIRECTORY names, in that order, so each lands
//  on the id Driver.h gives it:
//
//    DEFAULT  the root's when it has none stored: SYSTEM and Administrators
//             full control, Authenticated Users modify, Everyone read and
//             execute, all inherited by files and directories beneath.
//    LOCKED   SYSTEM and Administrators full control, inherited likewise.
//    DEFAULT_FILE, DEFAULT_DIRECTORY
//             what a file and a directory inherit from DEFAULT.
//
NTSTATUS BlorgSecurityInitialize(VOID)
{
    SecurityById = ExAllocatePoolZero(PagedPool, SECURITY_MAX_ENTRIES * sizeof(PSECURITY_ENTRY), SECURITY_TAG);

    if (!SecurityById)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExInitializePushLock(&SecurityLock);

    const ULONG inherit = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;

    const SECURITY_ACE_SPEC defaults[] =
    {
        { SeExports->SeLocalSystemSid, FILE_ALL_ACCESS, inherit },
        { SeExports->SeAliasAdminsSid, FILE_ALL_ACCESS, inherit },
        { SeExports->SeAuthenticatedUsersSid, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE, inherit },
        { SeExports->SeWorldSid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, inherit },
    };

    ULONG id = BLORGFS_SECURITY_UNKNOWN;
    NTSTATUS status = SecurityBuild(defaults, ARRAYSIZE(defaults), &id);

    if (NT_SUCCESS(status) && (BLORGFS_SECURITY_DEFAULT == id))
    {
        status = SecurityBuild(defaults, 2, &id);
    }

    if (NT_SUCCESS(status) && (BLORGFS_SECURITY_LOCKED == id) &&
        (BLORGFS_SECURITY_DEFAULT_FILE == BlorgSecurityInherit(BLORGFS_SECURITY_DEFAULT, 1, FALSE)) &&
        (BLORGFS_SECURITY_DEFAULT_DIRECTORY == BlorgSecurityInherit(BLORGFS_SECURITY_DEFAULT, 1, TRUE)))
    {
        return STATUS_SUCCESS;
    }

    BlorgSecurityCleanup();

    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

//
//  Frees every interned descriptor and the table. Called at driver unload,
//  and by a failed BlorgSecurityInitialize.
//
VOID BlorgSecurityCleanup(VOID)
{
    if (!SecurityById)
    {
        return;
    }

    for (ULONG i = 0; i < SecurityCount; i++)
    {
        ExFreePool(SecurityById[i]);
    }

    ExFreePool(SecurityById);
    SecurityById = NULL;
    SecurityCount = 0;
    SecurityBytes = 0;
    RtlZeroMemory(SecurityBuckets, sizeof(SecurityBuckets));
}

//
//  The access check an open of a node resolving to Id makes, the way NTFS
//  makes it: against what the I/O manager has not already granted (backup
//  and restore intent are granted before the request arrives), recording
//  what is granted and any privilege used on the access state, which is
//  what the handle is given. Nothing left to grant needs no check. A
//  kernel-mode caller is trusted unless it asked to be checked
//  (SL_FORCE_ACCESS_CHECK), as everywhere else in the kernel.
//
NTSTATUS BlorgSecurityCheckOpen(ULONG Id, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    if (!FlagOn(irpSp->Flags, SL_FORCE_ACCESS_CHECK) && (KernelMode == Irp->RequestorMode))
    {
        return STATUS_SUCCESS;
    }

    PACCESS_STATE accessState = irpSp->Parameters.Create.SecurityContext->AccessState;

    if (0 == accessState->RemainingDesiredAccess)
    {
        return STATUS_SUCCESS;
    }

    PPRIVILEGE_SET privileges = NULL;
    ACCESS_MASK granted = 0;
    NTSTATUS status = STATUS_ACCESS_DENIED;

    SeLockSubjectContext(&accessState->SubjectSecurityContext);

    const BOOLEAN allowed = SeAccessCheck(
        SecurityDescriptor(Id),
        &accessState->SubjectSecurityContext,
        TRUE,
        accessState->RemainingDesiredAccess,
        accessState->PreviouslyGrantedAccess,
        &privileges,
        IoGetFileObjectGenericMapping(),
        UserMode,
        &granted,
        &status);

    if (privileges)
    {
        (VOID)SeAppendPrivileges(accessState, privileges);
        SeFreePrivileges(privileges);
    }

    if (allowed)
    {
        accessState->PreviouslyGrantedAccess |= granted;
        accessState->RemainingDesiredAccess &= ~(granted | MAXIMUM_ALLOWED);
    }

    SeUnlockSubjectContext(&accessState->SubjectSecurityContext);

    return allowed ? STATUS_SUCCESS : status;
}

//
// Volume IRP_MJ_QUERY_SECURITY handler: returns the requested components of
// the descriptor the handle's node resolves to into the IRP's user buffer
// via SeQuerySecurityDescriptorInfo, guarded by SEH since the buffer is
// user-supplied. A volume handle is answered with the root's default.
//
static NTSTATUS SecurityQueryVolume(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    SECURITY_INFORMATION securityInformation = IrpSp->Parameters.QuerySecurity.SecurityInformation;
    ULONG length = IrpSp->Parameters.QuerySecurity.Length;
    PVOID node = IrpSp->FileObject->FsContext;
    ULONG id = BLORGFS_SECURITY_DEFAULT;

    if (node && (BLORGFS_VCB_SIGNATURE != GET_NODE_TYPE(node)))
    {
        id = C_CAST(ULONG, ReadNoFence(C_CAST(LONG*, &C_CAST(PCOMMON_CONTEXT, node)->SecurityId)));
    }

    PSECURITY_DESCRIPTOR objectSd = SecurityDescriptor(id);

    NTSTATUS status;

    __try
    {
        status = SeQuerySecurityDescriptorInfo(
            &securityInformation,
            Irp->UserBuffer,
            &length,
            &objectSd);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        length = 0;
    }

    Irp->IoStatus.Information = length;
    return status;
}

//
// IRP_MJ_QUERY_SECURITY dispatch entry point: routes to
// SecurityQueryVolume for the volume device object and completes the
// IRP with the result (disk/FS-control device objects are unimplemented).
//
NTSTATUS BlorgQuerySecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            result = SecurityQueryVolume(Irp, irpSp);
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return result;
}

//
// IRP_MJ_SET_SECURITY dispatch entry point: unimplemented for every device
// object type, always completes with STATUS_INVALID_DEVICE_REQUEST.
//
NTSTATUS BlorgSetSecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return result;
}
