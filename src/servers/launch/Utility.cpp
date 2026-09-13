/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include "Utility.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <device/scsi.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <FindDirectory.h>
#include <fs_info.h>
#include <Path.h>
#include <Volume.h>


namespace {


status_t
IssueDeviceCommand(const char* path, int opcode, void* buffer,
	size_t bufferSize)
{
	fs_info info;
	if (fs_stat_dev(dev_for_path(path), &info) == B_OK) {
		if (strcmp(info.fsh_name, "devfs") != 0)
			path = info.device_name;
	}

	int device = open(path, O_RDONLY);
	if (device < 0)
		return device;

	status_t status = B_OK;

	if (ioctl(device, opcode, buffer, bufferSize) != 0) {
		fprintf(stderr, "Failed to process %d on %s: %s\n", opcode, path,
			strerror(errno));
		status = errno;
	}
	close(device);
	return status;
}


}	// private namespace


namespace Utility {


bool
IsReadOnlyVolume(dev_t device)
{
	BVolume volume;
	status_t status = volume.SetTo(device);
	if (status != B_OK) {
		fprintf(stderr, "Failed to get BVolume for device %" B_PRIdDEV
			": %s\n", device, strerror(status));
		return false;
	}

	BDiskDeviceRoster roster;
	BDiskDevice diskDevice;
	BPartition* partition;
	status = roster.FindPartitionByVolume(volume, &diskDevice, &partition);
	if (status != B_OK) {
		fprintf(stderr, "Failed to get partition for device %" B_PRIdDEV
			": %s\n", device, strerror(status));
		return false;
	}

	return partition->IsReadOnly();
}


bool
IsReadOnlyVolume(const char* path)
{
	return IsReadOnlyVolume(dev_for_path(path));
}


/*!	Whether the volume is on a drive meant to be carried from machine to
	machine: one with removable media, or one attached over USB, which is where
	a pen drive shows up whether or not it calls its media removable.
*/
bool
IsRemovableVolume(const char* path)
{
	BVolume volume;
	if (volume.SetTo(dev_for_path(path)) != B_OK)
		return false;

	BDiskDeviceRoster roster;
	BDiskDevice diskDevice;
	BPartition* partition;
	if (roster.FindPartitionByVolume(volume, &diskDevice, &partition) != B_OK)
		return false;

	BPath devicePath;
	return diskDevice.IsRemovableMedia()
		|| (diskDevice.GetPath(&devicePath) == B_OK
			&& strncmp(devicePath.Path(), "/dev/disk/usb/", 14) == 0);
}


status_t
BlockMedia(const char* path, bool block)
{
	return IssueDeviceCommand(path, B_SCSI_PREVENT_ALLOW, &block,
		sizeof(block));
}


status_t
EjectMedia(const char* path)
{
	return IssueDeviceCommand(path, B_EJECT_DEVICE, NULL, 0);
}


BString
TranslatePath(const char* originalPath)
{
	BString path = originalPath;

	// The home of the user whose jobs these are: a session daemon has HOME
	// set from the account before it reads its jobs. Without it, this is the
	// system daemon, running as uid 0.
	BString home = getenv("HOME");
	if (home.IsEmpty()) {
		BPath homePath;
		home = find_directory(B_USER_DIRECTORY, &homePath) == B_OK
			? homePath.Path() : "/boot/home";
	}
	path.ReplaceAll("$HOME", home);
	path.ReplaceAll("${HOME}", home);
	if (path.StartsWith("~/"))
		path.ReplaceFirst("~", home);

	return path;
}


}	// namespace Utility
