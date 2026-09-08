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
#define INTEL_VERSION_RETRIES	5
#define INTEL_VERSION_RETRY_DELAY	100000


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
	sem_id			semaphore;
	status_t		status;
	size_t			length;
	bool			pending;
	bool			ready;
	uint8			data[260];
};


/*!	The two endpoints an event can arrive on.

	While the controller runs its bootloader it answers the secure send
	command on the bulk endpoint and everything else on the interrupt
	endpoint, so a command has to listen on both of them and take whichever
	one replies. The Linux btusb driver feeds what arrives on either endpoint
	into the same event handling for the same reason.
*/
struct intel_event_source {
	sem_id				semaphore;
	intel_event_wait	interrupt;
	intel_event_wait	bulk;
};


static void
intel_event_complete(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	intel_event_wait* wait = (intel_event_wait*)cookie;
	wait->status = status;
	wait->length = min_c(actualLength, sizeof(wait->data));
	wait->pending = false;
	wait->ready = true;
	release_sem_etc(wait->semaphore, 1, B_DO_NOT_RESCHEDULE);
}


struct intel_transfer_wait {
	sem_id			semaphore;
	status_t		status;
	size_t			length;
};


static void
intel_transfer_complete(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	intel_transfer_wait* wait = (intel_transfer_wait*)cookie;
	wait->status = status;
	wait->length = actualLength;
	release_sem_etc(wait->semaphore, 1, B_DO_NOT_RESCHEDULE);
}


/*!	Sends a command packet through the bulk out endpoint.

	While the controller runs its bootloader it does not answer the secure
	send command on the control endpoint, so the firmware download has to go
	through the bulk endpoint instead. The Linux btusb driver treats the
	command the same way.
*/
static status_t
intel_bulk_command(bt_usb_dev* device, const void* command, size_t length)
{
	if (device->bulk_out_ep == NULL)
		return B_NOT_SUPPORTED;

	intel_transfer_wait wait;
	wait.semaphore = create_sem(0, "Intel Bluetooth command sent");
	if (wait.semaphore < B_OK)
		return wait.semaphore;

	wait.status = B_ERROR;
	wait.length = 0;
	status_t status = usb->queue_bulk(device->bulk_out_ep->handle,
		(void*)command, length, intel_transfer_complete, &wait);
	if (status == B_OK) {
		status = acquire_sem_etc(wait.semaphore, 1, B_RELATIVE_TIMEOUT,
			INTEL_COMMAND_TIMEOUT);
	}

	if (status != B_OK) {
		usb->cancel_queued_transfers(device->bulk_out_ep->handle);
		acquire_sem_etc(wait.semaphore, 1, B_RELATIVE_TIMEOUT,
			INTEL_COMMAND_TIMEOUT);
	} else
		status = wait.status;

	if (status == B_OK && wait.length != length) {
		ERROR("Intel bulk command sent %" B_PRIuSIZE " of %" B_PRIuSIZE
			" bytes\n", wait.length, length);
	}

	delete_sem(wait.semaphore);
	return status;
}


/*!	Sends a command packet on either of the two endpoints that can carry one.

	Commands normally travel on the control endpoint, but the bootloader wants
	the secure send command on the bulk endpoint instead.
*/
static status_t
intel_send_command_packet(bt_usb_dev* device, const uint8* command,
	size_t length, bool useControlEndpoint)
{
	if (!useControlEndpoint)
		return intel_bulk_command(device, command, length);

	size_t actualLength = 0;
	return usb->send_request(device->dev, USB_TYPE_CLASS, 0, 0, 0, length,
		(void*)command, &actualLength);
}


/*!	Logs the raw bytes of an HCI event.

	The only way to tell a truncated answer from an error status is to look
	at what actually arrived, and which endpoint it arrived on matters while
	the controller runs its bootloader.
*/
static void
intel_dump_packet(const char* what, uint16 opcode, const uint8* data,
	size_t length)
{
	char hex[3 * 24 + 1];
	size_t shown = min_c(length, (size_t)24);
	for (size_t i = 0; i < shown; i++)
		snprintf(hex + 3 * i, 4, " %02x", data[i]);
	hex[3 * shown] = '\0';
	ERROR("%s for 0x%04x: %" B_PRIuSIZE " bytes%s%s\n", what, opcode, length,
		hex, length > shown ? " ..." : "");
}


static status_t
intel_queue_event(bt_usb_dev* device, intel_event_wait* wait, bool useBulk)
{
	wait->status = B_ERROR;
	wait->length = 0;
	wait->ready = false;
	wait->pending = true;

	status_t status;
	if (useBulk) {
		status = usb->queue_bulk(device->bulk_in_ep->handle, wait->data,
			sizeof(wait->data), intel_event_complete, wait);
	} else {
		status = usb->queue_interrupt(device->intr_in_ep->handle, wait->data,
			sizeof(wait->data), intel_event_complete, wait);
	}

	if (status != B_OK)
		wait->pending = false;
	return status;
}


static status_t
intel_event_source_init(intel_event_source* source, bt_usb_dev* device)
{
	memset(source, 0, sizeof(*source));
	source->semaphore = create_sem(0, "Intel Bluetooth firmware event");
	if (source->semaphore < B_OK)
		return source->semaphore;

	source->interrupt.semaphore = source->semaphore;
	source->bulk.semaphore = source->semaphore;

	status_t status = intel_queue_event(device, &source->interrupt, false);
	if (status != B_OK) {
		delete_sem(source->semaphore);
		return status;
	}

	// Listening on the bulk endpoint as well is what makes the firmware
	// download work, but a controller which only ever answers on the
	// interrupt endpoint still gets to finish its setup without it.
	if (device->bulk_in_ep != NULL)
		intel_queue_event(device, &source->bulk, true);

	return B_OK;
}


static void
intel_event_source_cleanup(intel_event_source* source, bt_usb_dev* device)
{
	if (source->interrupt.pending)
		usb->cancel_queued_transfers(device->intr_in_ep->handle);
	if (source->bulk.pending)
		usb->cancel_queued_transfers(device->bulk_in_ep->handle);

	// The transfers write into this structure, so none of them may still be
	// on its way out once it goes out of scope.
	while (source->interrupt.pending || source->bulk.pending) {
		if (acquire_sem_etc(source->semaphore, 1, B_RELATIVE_TIMEOUT,
				INTEL_COMMAND_TIMEOUT) != B_OK) {
			ERROR("Intel Bluetooth event transfers did not come back\n");
			break;
		}
	}

	delete_sem(source->semaphore);
}


/*!	Waits for the next event on either of the two endpoints. */
static status_t
intel_event_source_wait(intel_event_source* source, intel_event_wait** _event)
{
	status_t status = acquire_sem_etc(source->semaphore, 1,
		B_RELATIVE_TIMEOUT, INTEL_COMMAND_TIMEOUT);
	if (status != B_OK)
		return status;

	intel_event_wait* event = NULL;
	if (source->interrupt.ready)
		event = &source->interrupt;
	else if (source->bulk.ready)
		event = &source->bulk;
	if (event == NULL)
		return B_ERROR;

	event->ready = false;
	*_event = event;
	return event->status;
}


/*! Waits for an HCI event, ignoring stale ACL data on the bulk endpoint.

	The same bulk endpoint carries command replies in Intel bootloader mode and
	ACL data in operational mode. A warm reboot can leave ACL packets queued in
	the controller; those must not be mistaken for the reply to Read Version.
*/
static status_t
intel_event_source_wait_for_hci(intel_event_source* source,
	bt_usb_dev* device, intel_event_wait** _event)
{
	for (;;) {
		intel_event_wait* event;
		status_t status = intel_event_source_wait(source, &event);
		if (status != B_OK)
			return status;

		if (event != &source->bulk || (event->length != 0
				&& (event->data[0] == HCI_COMMAND_COMPLETE
					|| event->data[0] == HCI_VENDOR_EVENT))) {
			*_event = event;
			return B_OK;
		}

		intel_dump_packet("discarding stale bulk data", 0, event->data,
			event->length);
		status = intel_queue_event(device, event, true);
		if (status != B_OK)
			return status;
	}
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
	size_t commandLength = sizeof(uint16) + sizeof(uint8) + parameterLength;

	intel_event_source source;
	status_t status = intel_event_source_init(&source, device);
	if (status != B_OK)
		return status;

	// The bootloader wants the secure send command on the bulk endpoint;
	// every other command goes to the control endpoint.
	status = intel_send_command_packet(device, command, commandLength,
		opcode != INTEL_SECURE_SEND);
	if (status != B_OK)
		goto done;

	for (;;) {
		intel_event_wait* event;
		status = intel_event_source_wait_for_hci(&source, device, &event);
		if (status != B_OK) {
			ERROR("Intel command 0x%04x event wait failed: %s\n", opcode,
				strerror(status));
			goto done;
		}

		if (event->length >= 7 && event->data[0] == HCI_VENDOR_EVENT
			&& event->data[2] == INTEL_DOWNLOAD_COMPLETE) {
			// The controller announces the end of the download on its own,
			// possibly before the last fragment has been acknowledged.
			device->intelDownloadComplete = true;
			device->intelDownloadResult = event->data[3];
			status = intel_queue_event(device, event,
				event == &source.bulk);
			if (status != B_OK)
				goto done;
			continue;
		}

		if (event->length < 5 || event->data[0] != HCI_COMMAND_COMPLETE
			|| event->data[3] != (opcode & 0xff)
			|| event->data[4] != (opcode >> 8)) {
			intel_dump_packet("unexpected event", opcode, event->data,
				event->length);
			status = B_BAD_DATA;
			goto done;
		}

		if (opcode != INTEL_SECURE_SEND) {
			intel_dump_packet("command complete", opcode, event->data,
				event->length);
		}

		size_t actualLength = event->length - 5;
		if (responseLength != NULL) {
			if (response != NULL && actualLength > *responseLength) {
				status = B_BUFFER_OVERFLOW;
				goto done;
			}
			if (response != NULL && actualLength != 0)
				memcpy(response, event->data + 5, actualLength);
			*responseLength = actualLength;
		}

		status = B_OK;
		break;
	}

done:
	intel_event_source_cleanup(&source, device);
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


/*!	Takes the next event and checks that it is the vendor event we want. */
static status_t
intel_expect_vendor_event(intel_event_source* source, bt_usb_dev* device,
	uint8 subtype)
{
	intel_event_wait* event;
	status_t status = intel_event_source_wait_for_hci(source, device, &event);
	if (status != B_OK) {
		ERROR("waiting for the Intel vendor event %u failed: %s\n", subtype,
			strerror(status));
		return status;
	}

	intel_dump_packet("vendor event", subtype, event->data, event->length);
	if (event->length < 3 || event->data[0] != HCI_VENDOR_EVENT
		|| event->data[2] != subtype) {
		return B_BAD_DATA;
	}

	// Both events this is used for carry a status byte the controller uses to
	// report that it did not like what it was given.
	if (event->length < 4 || event->data[3] != 0)
		return B_ERROR;

	return B_OK;
}


static status_t
intel_wait_for_vendor_event(bt_usb_dev* device, uint8 subtype)
{
	if (subtype == INTEL_DOWNLOAD_COMPLETE
		&& device->intelDownloadComplete) {
		return device->intelDownloadResult == 0 ? B_OK : B_ERROR;
	}

	intel_event_source source;
	status_t status = intel_event_source_init(&source, device);
	if (status != B_OK)
		return status;

	status = intel_expect_vendor_event(&source, device, subtype);
	intel_event_source_cleanup(&source, device);
	return status;
}


/*!	Boots the controller into the firmware that was just downloaded.

	The reset command is not answered with a command complete event: the
	controller restarts and announces itself with the boot complete event
	instead, so waiting for a command complete would only ever time out. The
	Linux btusb driver makes up the missing event for its own flow control.

	The transfers the event will arrive on are queued before the command goes
	out, so that a controller which comes back quickly cannot slip past us.
*/
static status_t
intel_boot_firmware(bt_usb_dev* device, uint32 bootAddress)
{
	uint8 command[3 + 8] = { INTEL_RESET & 0xff, INTEL_RESET >> 8, 8,
		0x00, 0x01, 0x00, 0x01 };
	uint32 littleEndianBootAddress = B_HOST_TO_LENDIAN_INT32(bootAddress);
	memcpy(command + 7, &littleEndianBootAddress,
		sizeof(littleEndianBootAddress));

	intel_event_source source;
	status_t status = intel_event_source_init(&source, device);
	if (status != B_OK)
		return status;

	status = intel_send_command_packet(device, command, sizeof(command),
		true);
	if (status == B_OK)
		status = intel_expect_vendor_event(&source, device,
			INTEL_BOOT_COMPLETE);

	intel_event_source_cleanup(&source, device);
	return status;
}


status_t
intel_bluetooth_setup(bt_usb_dev* device)
{
	// The legacy version command of this generation of controllers takes no
	// parameter; the 0xff parameter selects the TLV answer of the later ones.
	// The controller answers the first commands it is sent with "command
	// disallowed" for as long as it is still settling after having been
	// configured, so it gets a few tries before we give up on it.
	intel_version version;
	size_t responseLength = 0;
	status_t status = B_ERROR;
	for (int attempt = 0; attempt < INTEL_VERSION_RETRIES; attempt++) {
		if (attempt != 0)
			snooze(INTEL_VERSION_RETRY_DELAY);

		memset(&version, 0, sizeof(version));
		responseLength = sizeof(version);
		status = intel_command(device, INTEL_READ_VERSION, NULL, 0,
			&version, &responseLength);
		if (status == B_OK && responseLength == sizeof(version)
			&& version.status == 0) {
			break;
		}

		ERROR("could not read Intel Bluetooth version (%s), attempt %d: %"
			B_PRIuSIZE " of %" B_PRIuSIZE " bytes, HCI status 0x%02x\n",
			strerror(status), attempt + 1, responseLength, sizeof(version),
			version.status);
	}

	if (status != B_OK || responseLength != sizeof(version)
		|| version.status != 0 || version.hardwarePlatform != 0x37) {
		ERROR("giving up on the Intel Bluetooth version, platform 0x%02x, "
			"hw variant 0x%02x, fw variant 0x%02x\n", version.hardwarePlatform,
			version.hardwareVariant, version.firmwareVariant);
		return status == B_OK ? B_BAD_DATA : status;
	}

	ERROR("Intel Bluetooth controller: hw variant 0x%02x revision 0x%02x, "
		"fw variant 0x%02x revision 0x%02x, build %u week %u year %u, "
		"patch %u\n", version.hardwareVariant, version.hardwareRevision,
		version.firmwareVariant, version.firmwareRevision,
		version.firmwareBuildNumber, version.firmwareBuildWeek,
		version.firmwareBuildYear, version.firmwarePatchNumber);

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

	status = intel_boot_firmware(device, bootAddress);
	if (status != B_OK) {
		ERROR("booting Intel Bluetooth firmware failed: %s\n",
			strerror(status));
		return status;
	}

	ERROR("loaded Intel Bluetooth firmware %s\n", firmwareName);
	return B_OK;
}
