/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <Application.h>
#include <ObjectList.h>
#include <OS.h>

#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/bdaddrUtils.h>

#include <stdio.h>
#include <string.h>


int
main(int argc, char** argv)
{
	BApplication application("application/x-vnd.Haiku-bluetooth-audio-connect");
	Bluetooth::LocalDevice* local = Bluetooth::LocalDevice::GetLocalDevice();
	if (local == NULL) {
		fprintf(stderr, "No local Bluetooth device\n");
		return 1;
	}

	BObjectList<Bluetooth::RemoteDevice> devices
		= Bluetooth::RemoteDevice::GetRemoteDevices(local);
	if (devices.IsEmpty()) {
		fprintf(stderr, "No known Bluetooth devices\n");
		return 1;
	}

	Bluetooth::RemoteDevice* selected = NULL;
	for (int32 i = 0; i < devices.CountItems(); i++) {
		Bluetooth::RemoteDevice* device = devices.ItemAt(i);
		BString address = Bluetooth::bdaddrUtils::ToString(
			device->GetBluetoothAddress());
		printf("%s  %s  state=%d\n", address.String(),
			device->GetCachedFriendlyName().String(), device->GetConnectionState());
		if (selected == NULL && (argc < 2 || strcasecmp(argv[1], address) == 0))
			selected = device;
	}
	if (selected == NULL) {
		fprintf(stderr, "Requested Bluetooth device is not known\n");
		return 1;
	}

	if (selected->GetConnectionState() != Bluetooth::RemoteDevice::CONNECTED) {
		status_t status = selected->Connect();
		if (status != B_OK) {
			fprintf(stderr, "Connect request failed: %s\n", strerror(status));
			return 1;
		}
	}
	for (int i = 0; i < 150; i++) {
		if (selected->GetConnectionState() == Bluetooth::RemoteDevice::CONNECTED) {
			puts("Bluetooth device connected");
			return 0;
		}
		snooze(100000);
	}
	fprintf(stderr, "Bluetooth connection timed out\n");
	return 1;
}
