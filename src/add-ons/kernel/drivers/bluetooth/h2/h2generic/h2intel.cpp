/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel secure firmware loading follows the protocol documented by the
 * Linux btintel driver.  The firmware image itself is redistributable but is
 * kept outside the driver, in data/firmware/h2generic.
 */

#include "h2intel.h"

#include <ByteOrder.h>
#include <FindDirectory.h>
#include <StorageDefs.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "h2debug.h"


#define INTEL_READ_VERSION		0xfc05
#define INTEL_READ_BOOT_PARAMS	0xfc0d
#define INTEL_SECURE_SEND		0xfc09
#define INTEL_RESET				0xfc01
#define INTEL_WRITE_BOOT_PARAMS	0xfc0e

#define HCI_COMMAND_COMPLETE		0x0e
#define HCI_VENDOR_EVENT			0xff
#define INTEL_DOWNLOAD_COMPLETE	0x06
#define INTEL_BOOT_COMPLETE		0x02

#define INTEL_RSA_HEADER_SIZE	644
#define INTEL_COMMAND_TIMEOUT	5000000


struct intel_version {
	uint8 status;
	uint8 hardwarePlatform;
	uint8 hardwareVariant;
	uint8 hardwareRevision;
	uint8 firmwareVariant;
	uint8 firmwareRevision;
	uint8 firmwareBuildNumber;
	uint8 firmwareBuildWeek;
	uint8 firmwareBuildYear;
	uint8 firmwarePatchNumber;
} _PACKED;


struct intel_boot_params {
	uint8 status;
	uint8 otpFormat;
	uint8 otpContent;
	uint8 otpPatch;
	uint16 deviceRevision;
	uint8 secureBoot;
	uint8 keyFromHeader;
	uint8 keyType;
	uint8 otpLock;
	uint8 apiLock;
	uint8 debugLock;
	uint8 otpAddress[6];
	uint8 minimumBuildNumber;
	uint8 minimumBuildWeek;
	uint8 minimumBuildYear;
	uint8 limitedCommandCompleteEvents;
	uint8 unlockedState;
} _PACKED;


struct intel_event_wait {
	sem_id semaphore;
	status_t status;
	size_t length;
	uint8 data[260];
};


static void
intel_event_complete(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	intel_event_wait* wait = (intel_event_wait*)cookie;
	wait->status = status;
	wait->length = min_c(actualLength, sizeof(wait->data));
	if (status == B_OK && wait->length != 0)
		memcpy(wait->data, data, wait->length);
	release_sem_etc(wait->semaphore, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
intel_wait_for_event(bt_usb_dev* device, intel_event_wait* wait)
{
	wait->semaphore = create_sem(0, "Intel Bluetooth firmware event");
	if (wait->semaphore < B_OK)
		return wait->semaphore;

	wait->status = B_ERROR;
	wait->length = 0;
	status_t status = usb->queue_interrupt(device->intr_in_ep->handle,
		wait->data, sizeof(wait->data), intel_event_complete, wait);
	if (status == B_OK) {
		status = acquire_sem_etc(wait->semaphore, 1, B_RELATIVE_TIMEOUT,
			INTEL_COMMAND_TIMEOUT);
	}

	if (status != B_OK) {
		usb->cancel_queued_transfers(device->intr_in_ep->handle);
		acquire_sem_etc(wait->semaphore, 1, B_RELATIVE_TIMEOUT,
			INTEL_COMMAND_TIMEOUT);
	} else
		status = wait->status;

	delete_sem(wait->semaphore);
	return status;
}


static status_t
intel_command(bt_usb_dev* device, uint16 opcode, const void* parameters,
	uint8 parameterLength, void* response, size_t* responseLength)
{
	uint8 command[3 + 255];
	command[0] = opcode & 0xff;
	command[1] = opcode >> 8;
	command[2] = parameterLength;
	if (parameterLength != 0)
		memcpy(command + 3, parameters, parameterLength);

	intel_event_wait wait;
	wait.semaphore = create_sem(0, "Intel Bluetooth command event");
	if (wait.semaphore < B_OK)
		return wait.semaphore;
	wait.status = B_ERROR;
	wait.length = 0;
	size_t actualLength = 0;

	status_t status = usb->queue_interrupt(device->intr_in_ep->handle,
		wait.data, sizeof(wait.data), intel_event_complete, &wait);
	if (status != B_OK)
		goto done;

	status = usb->send_request(device->dev, USB_TYPE_CLASS, 0, 0, 0,
		sizeof(uint16) + sizeof(uint8) + parameterLength, command,
		&actualLength);
	if (status != B_OK)
		goto cancel;

	for (;;) {
		status = acquire_sem_etc(wait.semaphore, 1, B_RELATIVE_TIMEOUT,
			INTEL_COMMAND_TIMEOUT);
		if (status != B_OK) {
			ERROR("Intel command 0x%04x event wait failed: %s\n", opcode,
				strerror(status));
			goto cancel;
		}
		if (wait.status != B_OK) {
			status = wait.status;
			goto done;
		}

		if (wait.length >= 7 && wait.data[0] == HCI_VENDOR_EVENT
			&& wait.data[2] == INTEL_DOWNLOAD_COMPLETE) {
			device->intelDownloadComplete = true;
			device->intelDownloadResult = wait.data[3];
			wait.status = B_ERROR;
			wait.length = 0;
			status = usb->queue_interrupt(device->intr_in_ep->handle,
				wait.data, sizeof(wait.data), intel_event_complete, &wait);
			if (status != B_OK)
				goto done;
			continue;
		}

		if (wait.length < 5 || wait.data[0] != HCI_COMMAND_COMPLETE
			|| wait.data[3] != (opcode & 0xff)
			|| wait.data[4] != (opcode >> 8)) {
			ERROR("Intel command 0x%04x returned an unexpected event\n",
				opcode);
			status = B_BAD_DATA;
			goto done;
		}
		break;
	}

	actualLength = wait.length - 5;
	if (responseLength != NULL) {
		if (response != NULL && actualLength > *responseLength) {
			status = B_BUFFER_OVERFLOW;
			goto done;
		}
		if (response != NULL && actualLength != 0)
			memcpy(response, wait.data + 5, actualLength);
		*responseLength = actualLength;
	}
	status = B_OK;
	goto done;

cancel:
	usb->cancel_queued_transfers(device->intr_in_ep->handle);
	acquire_sem_etc(wait.semaphore, 1, B_RELATIVE_TIMEOUT,
		INTEL_COMMAND_TIMEOUT);
done:
	delete_sem(wait.semaphore);
	return status;
}


static status_t
intel_secure_send(bt_usb_dev* device, uint8 type, const uint8* data,
	size_t length)
{
	while (length != 0) {
		uint8 parameters[253];
		uint8 fragmentLength = min_c(length, 252);
		parameters[0] = type;
		memcpy(parameters + 1, data, fragmentLength);

		uint8 result = 0xff;
		size_t resultLength = sizeof(result);
		status_t status = intel_command(device, INTEL_SECURE_SEND, parameters,
			fragmentLength + 1, &result, &resultLength);
		if (status != B_OK) {
			ERROR("Intel secure-send type %u failed with %lu bytes left\n",
				type, length);
			return status;
		}
		if (resultLength != 1 || result != 0)
			return B_ERROR;

		data += fragmentLength;
		length -= fragmentLength;
	}
	return B_OK;
}


static status_t
intel_load_firmware(const char* name, uint8** _data, size_t* _size)
{
	const directory_which directories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, B_SYSTEM_DATA_DIRECTORY
	};
	char path[B_PATH_NAME_LENGTH];
	int fd = -1;

	for (size_t i = 0; i < B_COUNT_OF(directories); i++) {
		if (find_directory(directories[i], -1, false, path, sizeof(path))
			!= B_OK) {
			continue;
		}
		strlcat(path, "/firmware/h2generic/", sizeof(path));
		strlcat(path, name, sizeof(path));
		fd = open(path, O_RDONLY);
		if (fd >= 0)
			break;
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	if (size < INTEL_RSA_HEADER_SIZE || lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	*_data = data;
	*_size = size;
	return B_OK;
}


static status_t
intel_find_boot_address(const uint8* firmware, size_t firmwareSize,
	uint32* bootAddress)
{
	size_t offset = INTEL_RSA_HEADER_SIZE;
	while (offset + 3 <= firmwareSize) {
		uint16 opcode = firmware[offset] | ((uint16)firmware[offset + 1] << 8);
		uint8 length = firmware[offset + 2];
		if (offset + 3 + length > firmwareSize)
			return B_BAD_DATA;

		if (opcode == INTEL_WRITE_BOOT_PARAMS && length >= 4) {
			memcpy(bootAddress, firmware + offset + 3, sizeof(*bootAddress));
			*bootAddress = B_LENDIAN_TO_HOST_INT32(*bootAddress);
			return B_OK;
		}
		offset += 3 + length;
	}
	return B_BAD_DATA;
}


static status_t
intel_upload_firmware(bt_usb_dev* device, const uint8* firmware,
	size_t firmwareSize)
{
	status_t status = intel_secure_send(device, 0x00, firmware, 128);
	if (status != B_OK)
		return status;
	status = intel_secure_send(device, 0x03, firmware + 128, 256);
	if (status != B_OK)
		return status;
	status = intel_secure_send(device, 0x02, firmware + 388, 256);
	if (status != B_OK)
		return status;

	ERROR("Intel Bluetooth firmware header accepted\n");
	size_t offset = INTEL_RSA_HEADER_SIZE;
	while (offset < firmwareSize) {
		size_t fragmentLength = 0;
		do {
			if (offset + fragmentLength + 3 > firmwareSize)
				return B_BAD_DATA;
			uint8 commandLength = firmware[offset + fragmentLength + 2];
			fragmentLength += 3 + commandLength;
			if (offset + fragmentLength > firmwareSize)
				return B_BAD_DATA;
		} while ((fragmentLength & 3) != 0);

		status = intel_secure_send(device, 0x01, firmware + offset,
			fragmentLength);
		if (status != B_OK)
			return status;
		offset += fragmentLength;
		if ((offset & 0xffff) < fragmentLength)
			ERROR("Intel Bluetooth firmware upload: %lu/%lu bytes\n",
				offset, firmwareSize);
	}
	return B_OK;
}


static status_t
intel_wait_for_vendor_event(bt_usb_dev* device, uint8 subtype)
{
	if (subtype == INTEL_DOWNLOAD_COMPLETE
		&& device->intelDownloadComplete) {
		return device->intelDownloadResult == 0 ? B_OK : B_ERROR;
	}

	intel_event_wait wait;
	status_t status = intel_wait_for_event(device, &wait);
	if (status != B_OK)
		return status;
	if (wait.length < 3 || wait.data[0] != HCI_VENDOR_EVENT
		|| wait.data[2] != subtype) {
		return B_BAD_DATA;
	}
	if (subtype == INTEL_DOWNLOAD_COMPLETE
		&& (wait.length < 7 || wait.data[3] != 0)) {
		return B_ERROR;
	}
	return B_OK;
}


status_t
intel_bluetooth_setup(bt_usb_dev* device)
{
	intel_version version;
	size_t responseLength = sizeof(version);
	uint8 parameter = 0xff;
	status_t status = intel_command(device, INTEL_READ_VERSION, &parameter,
		1, &version, &responseLength);
	if (status != B_OK || responseLength != sizeof(version)
		|| version.status != 0 || version.hardwarePlatform != 0x37) {
		ERROR("could not read Intel Bluetooth version (%s)\n",
			strerror(status));
		return status == B_OK ? B_BAD_DATA : status;
	}

	if (version.firmwareVariant == 0x23)
		return B_OK;
	if (version.firmwareVariant != 0x06 || version.hardwareVariant != 0x0b) {
		ERROR("unsupported Intel Bluetooth variant hw=%u fw=%u\n",
			version.hardwareVariant, version.firmwareVariant);
		return B_NOT_SUPPORTED;
	}

	intel_boot_params bootParams;
	responseLength = sizeof(bootParams);
	status = intel_command(device, INTEL_READ_BOOT_PARAMS, NULL, 0,
		&bootParams, &responseLength);
	if (status != B_OK || responseLength != sizeof(bootParams)
		|| bootParams.status != 0 || bootParams.limitedCommandCompleteEvents != 0) {
		ERROR("could not read supported Intel Bluetooth boot parameters\n");
		return status == B_OK ? B_BAD_DATA : status;
	}

	uint16 deviceRevision
		= B_LENDIAN_TO_HOST_INT16(bootParams.deviceRevision);
	char firmwareName[32];
	snprintf(firmwareName, sizeof(firmwareName), "ibt-%u-%u.sfi",
		version.hardwareVariant, deviceRevision);

	uint8* firmware = NULL;
	size_t firmwareSize = 0;
	status = intel_load_firmware(firmwareName, &firmware, &firmwareSize);
	if (status != B_OK) {
		ERROR("Intel Bluetooth firmware %s was not found\n", firmwareName);
		return status;
	}

	uint32 bootAddress = 0;
	status = intel_find_boot_address(firmware, firmwareSize, &bootAddress);
	if (status == B_OK)
		status = intel_upload_firmware(device, firmware, firmwareSize);
	free(firmware);
	if (status != B_OK) {
		ERROR("uploading Intel Bluetooth firmware failed: %s\n",
			strerror(status));
		return status;
	}

	status = intel_wait_for_vendor_event(device, INTEL_DOWNLOAD_COMPLETE);
	if (status != B_OK) {
		ERROR("Intel Bluetooth firmware verification failed: %s\n",
			strerror(status));
		return status;
	}

	uint8 reset[8] = { 0x00, 0x01, 0x00, 0x01 };
	uint32 littleEndianBootAddress = B_HOST_TO_LENDIAN_INT32(bootAddress);
	memcpy(reset + 4, &littleEndianBootAddress, sizeof(littleEndianBootAddress));
	responseLength = 0;
	status = intel_command(device, INTEL_RESET, reset, sizeof(reset), NULL,
		&responseLength);
	if (status == B_OK)
		status = intel_wait_for_vendor_event(device, INTEL_BOOT_COMPLETE);
	if (status != B_OK) {
		ERROR("booting Intel Bluetooth firmware failed: %s\n",
			strerror(status));
		return status;
	}

	ERROR("loaded Intel Bluetooth firmware %s\n", firmwareName);
	return B_OK;
}
