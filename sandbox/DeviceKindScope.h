#pragma once

//
// Points one of the driver's three device-object globals at a synthetic
// device for the duration of a scope, then puts it back.
//
// BlorgDeviceKind (Driver.h) decides what an incoming DEVICE_OBJECT is by
// comparing it against global.VolumeDeviceObject / DiskDeviceObject /
// FileSystemDeviceObject, so a test that wants its own device treated as
// one of those points the matching global at it. Restoring is not
// housekeeping: the globals outlive an individual test while the device
// objects it created do not, and a pointer left behind would classify
// whatever the allocator next placed at that address as the driver's own
// volume. Fixtures that hold a device for their whole lifetime set and
// clear the global in SetUp/TearDown instead; this is for the tests that
// need one only for a single call.
//
class ScopedDeviceKind
{
public:
    ScopedDeviceKind(PDEVICE_OBJECT* Slot, PDEVICE_OBJECT Device)
        : Slot(Slot), Saved(*Slot)
    {
        *Slot = Device;
    }

    ~ScopedDeviceKind()
    {
        *Slot = Saved;
    }

    ScopedDeviceKind(const ScopedDeviceKind&) = delete;
    ScopedDeviceKind& operator=(const ScopedDeviceKind&) = delete;

private:
    PDEVICE_OBJECT* Slot;
    PDEVICE_OBJECT Saved;
};
