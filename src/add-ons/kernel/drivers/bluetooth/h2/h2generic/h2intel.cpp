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
#define INTEL_ECDSA_HEADER_SIZE	320
#define INTEL_ECDSA_HEADER_OFFSET	INTEL_RSA_HEADER_SIZE
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


// Newer Intel controllers return this information as a stream of TLVs when
// Read Version is sent with the 0xff parameter.
struct intel_tlv_version {
	uint32	cnviTop;
	uint32	cnvrTop;
	uint32	cnviBluetooth;
	uint16	deviceRevision;
	uint8	imageType;
	uint8	limitedCommandCompleteEvents;
	uint8	secureBootEngineType;
};


enum {
	INTEL_TLV_CNVI_TOP = 0x10,
	INTEL_TLV_CNVR_TOP,
	INTEL_TLV_CNVI_BT,
	INTEL_TLV_CNVR_BT,
	INTEL_TLV_CNVI_OTP,
	INTEL_TLV_CNVR_OTP,
	INTEL_TLV_DEVICE_REVISION,
	INTEL_TLV_USB_VENDOR,
	INTEL_TLV_USB_PRODUCT,
	INTEL_TLV_PCIE_VENDOR,
	INTEL_TLV_PCIE_DEVICE,
	INTEL_TLV_PCIE_SUBSYSTEM,
	INTEL_TLV_IMAGE_TYPE,
	INTEL_TLV_TIMESTAMP,
	INTEL_TLV_BUILD_TYPE,
	INTEL_TLV_BUILD_NUMBER,
	INTEL_TLV_FIRMWARE_BUILD_PRODUCT,
	INTEL_TLV_FIRMWARE_BUILD_HARDWARE,
	INTEL_TLV_FIRMWARE_STEP,
	INTEL_TLV_BLUETOOTH_SPEC,
	INTEL_TLV_MANUFACTURER_NAME,
	INTEL_TLV_HCI_REVISION,
	INTEL_TLV_LMP_SUBVERSION,
	INTEL_TLV_OTP_PATCH_VERSION,
	INTEL_TLV_SECURE_BOOT,
	INTEL_TLV_KEY_FROM_HEADER,
	INTEL_TLV_OTP_LOCK,
	INTEL_TLV_API_LOCK,
	INTEL_TLV_DEBUG_LOCK,
	INTEL_TLV_MINIMUM_FIRMWARE,
	INTEL_TLV_LIMITED_COMMAND_COMPLETE,
	INTEL_TLV_SECURE_BOOT_ENGINE
};


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


static uint32
intel_read_le32(const uint8* data)
{
	uint32 value;
	memcpy(&value, data, sizeof(value));
	return B_LENDIAN_TO_HOST_INT32(value);
}


static uint16
intel_read_le16(const uint8* data)
{
	uint16 value;
	memcpy(&value, data, sizeof(value));
	return B_LENDIAN_TO_HOST_INT16(value);
}


/*! Read and validate the TLV form of Intel's version record. */
static status_t
intel_read_tlv_version(bt_usb_dev* device, intel_tlv_version* version)
{
	uint8 response[255];
	uint8 parameter = 0xff;
	size_t responseLength = sizeof(response);
	status_t status = intel_command(device, INTEL_READ_VERSION, &parameter,
		sizeof(parameter), response, &responseLength);
	if (status != B_OK)
		return status;
	if (responseLength < 1 || response[0] != 0)
		return B_BAD_DATA;

	memset(version, 0, sizeof(*version));
	bool haveCnviTop = false;
	bool haveCnvrTop = false;
	bool haveCnviBluetooth = false;
	bool haveImageType = false;
	for (size_t offset = 1; offset < responseLength;) {
		if (responseLength - offset < 2)
			return B_BAD_DATA;
		uint8 type = response[offset++];
		uint8 length = response[offset++];
		if (length > responseLength - offset)
			return B_BAD_DATA;

		const uint8* value = response + offset;
		switch (type) {
			case INTEL_TLV_CNVI_TOP:
				if (length != 4)
					return B_BAD_DATA;
				version->cnviTop = intel_read_le32(value);
				haveCnviTop = true;
				break;
			case INTEL_TLV_CNVR_TOP:
				if (length != 4)
					return B_BAD_DATA;
				version->cnvrTop = intel_read_le32(value);
				haveCnvrTop = true;
				break;
			case INTEL_TLV_CNVI_BT:
				if (length != 4)
					return B_BAD_DATA;
				version->cnviBluetooth = intel_read_le32(value);
				haveCnviBluetooth = true;
				break;
			case INTEL_TLV_DEVICE_REVISION:
				if (length != 2)
					return B_BAD_DATA;
				version->deviceRevision = intel_read_le16(value);
				break;
			case INTEL_TLV_IMAGE_TYPE:
				if (length != 1)
					return B_BAD_DATA;
				version->imageType = value[0];
				haveImageType = true;
				break;
			case INTEL_TLV_LIMITED_COMMAND_COMPLETE:
				if (length != 1)
					return B_BAD_DATA;
				version->limitedCommandCompleteEvents = value[0];
				break;
			case INTEL_TLV_SECURE_BOOT_ENGINE:
				if (length != 1)
					return B_BAD_DATA;
				version->secureBootEngineType = value[0];
				break;
		}
		offset += length;
	}

	if (!haveCnviTop || !haveCnvrTop || !haveCnviBluetooth || !haveImageType)
		return B_BAD_DATA;
	if (((version->cnviBluetooth & 0x0000ff00) >> 8) != 0x37)
		return B_NOT_SUPPORTED;
	if (version->limitedCommandCompleteEvents != 0
		|| version->secureBootEngineType > 1) {
		return B_NOT_SUPPORTED;
	}
	return B_OK;
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
	size_t payloadOffset, uint32* bootAddress)
{
	size_t offset = payloadOffset;
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
	size_t firmwareSize, size_t payloadOffset, bool useEcdsaHeader)
{
	if (payloadOffset > firmwareSize || firmwareSize - payloadOffset < 3)
		return B_BAD_DATA;

	const uint8* header = firmware;
	size_t publicKeyLength = 256;
	size_t signatureLength = 256;
	if (useEcdsaHeader) {
		if (firmwareSize < INTEL_ECDSA_HEADER_OFFSET + INTEL_ECDSA_HEADER_SIZE)
			return B_BAD_DATA;
		header += INTEL_ECDSA_HEADER_OFFSET;
		publicKeyLength = 96;
		signatureLength = 96;
	}

	status_t status = intel_secure_send(device, 0x00, header, 128);
	if (status != B_OK)
		return status;
	status = intel_secure_send(device, 0x03, header + 128, publicKeyLength);
	if (status != B_OK)
		return status;
	status = intel_secure_send(device, 0x02, header + 128 + publicKeyLength,
		signatureLength);
	if (status != B_OK)
		return status;

	ERROR("Intel Bluetooth firmware header accepted\n");
	size_t offset = payloadOffset;
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


static void
intel_clear_halted_pipe(const usb_endpoint_info* endpoint)
{
	if (endpoint == NULL)
		return;

	usb->cancel_queued_transfers(endpoint->handle);
	status_t status = usb->clear_feature(endpoint->handle,
		USB_FEATURE_ENDPOINT_HALT);
	if (status != B_OK)
		ERROR("clearing halt on Intel Bluetooth pipe failed: %s\n",
			strerror(status));
}


static uint16
intel_pack_top_name_component(uint32 top)
{
	uint16 component = ((top & 0x00000fff) << 4) | ((top >> 24) & 0x0f);
	return B_SWAP_INT16(component);
}


static status_t
intel_bluetooth_setup_tlv(bt_usb_dev* device)
{
	intel_tlv_version version;
	status_t status = intel_read_tlv_version(device, &version);
	if (status != B_OK) {
		ERROR("could not read Intel Bluetooth TLV version: %s\n",
			strerror(status));
		return status;
	}

	uint8 hardwareVariant = (version.cnviBluetooth & 0x003f0000) >> 16;
	ERROR("Intel Bluetooth TLV controller: hw variant 0x%02x, image 0x%02x, "
		"CNVi 0x%08" B_PRIx32 ", CNVR 0x%08" B_PRIx32 ", SBE %u\n",
		hardwareVariant, version.imageType, version.cnviTop, version.cnvrTop,
		version.secureBootEngineType);
	if (version.imageType == 0x03)
		return B_OK;
	if (version.imageType != 0x01 || hardwareVariant < 0x17)
		return B_NOT_SUPPORTED;

	char firmwareName[32];
	snprintf(firmwareName, sizeof(firmwareName), "ibt-%04x-%04x.sfi",
		intel_pack_top_name_component(version.cnviTop),
		intel_pack_top_name_component(version.cnvrTop));

	uint8* firmware = NULL;
	size_t firmwareSize = 0;
	status = intel_load_firmware(firmwareName, &firmware, &firmwareSize);
	if (status != B_OK) {
		ERROR("Intel Bluetooth firmware %s was not found\n", firmwareName);
		return status;
	}

	const size_t payloadOffset = INTEL_RSA_HEADER_SIZE + INTEL_ECDSA_HEADER_SIZE;
	uint32 bootAddress = 0;
	status = intel_find_boot_address(firmware, firmwareSize, payloadOffset,
		&bootAddress);
	if (status == B_OK) {
		status = intel_upload_firmware(device, firmware, firmwareSize,
			payloadOffset, version.secureBootEngineType == 1);
	}
	free(firmware);
	if (status != B_OK) {
		ERROR("uploading Intel Bluetooth firmware failed: %s\n",
			strerror(status));
		return status;
	}

	status = intel_wait_for_vendor_event(device, INTEL_DOWNLOAD_COMPLETE);
	if (status != B_OK)
		return status;
	status = intel_boot_firmware(device, bootAddress);
	if (status != B_OK)
		return status;

	intel_clear_halted_pipe(device->bulk_in_ep);
	intel_clear_halted_pipe(device->bulk_out_ep);
	ERROR("loaded Intel Bluetooth firmware %s\n", firmwareName);
	return B_OK;
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
		status_t tlvStatus = intel_bluetooth_setup_tlv(device);
		if (tlvStatus == B_OK)
			return B_OK;
		ERROR("giving up on the Intel Bluetooth version, platform 0x%02x, "
			"hw variant 0x%02x, fw variant 0x%02x\n", version.hardwarePlatform,
			version.hardwareVariant, version.firmwareVariant);
		return tlvStatus;
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
	status = intel_find_boot_address(firmware, firmwareSize,
		INTEL_RSA_HEADER_SIZE, &bootAddress);
	if (status == B_OK)
		status = intel_upload_firmware(device, firmware, firmwareSize,
			INTEL_RSA_HEADER_SIZE, false);
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

	// Rebooting into the firmware halts the bulk pipes that were listening
	// for events (xhci reports a USB transaction error), and a halted endpoint
	// never completes the ACL receive queued later. Only do this after an
	// actual reboot: clearing a pipe that is not halted resets the device's
	// data toggle but not the controller's, and ACL data is then dropped.
	intel_clear_halted_pipe(device->bulk_in_ep);
	intel_clear_halted_pipe(device->bulk_out_ep);

	ERROR("loaded Intel Bluetooth firmware %s\n", firmwareName);
	return B_OK;
}
