/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Attribute protocol client, and just enough of GATT to drive HID over GATT.

	A Low Energy keyboard or mouse exposes its input as characteristics of the
	HID service, and reports arrive as attribute notifications rather than on a
	channel of their own. Only boot protocol mode is used: it fixes the report
	layout, which saves parsing the report map and matches what the input
	server add-on already understands from classic devices.
*/

#include "att.h"

#include "l2cap_internal.h"
#include "smp.h"

#include <bluetooth/l2cap.h>
#include <BluetoothHID.h>
#include <btCoreData.h>
#include <btDebug.h>

#include <NetBufferUtilities.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <string.h>


enum att_state {
	kIdle,
	kExchangingMtu,
	kFindingService,
	kFindingCharacteristics,
	kFindingDescriptors,
	kSettingBootMode,
	kEnablingKeyboard,
	kEnablingMouse,
	// Report protocol mode, for a peripheral that offers no boot reports.
	kReadingReportMap,
	kFindingReportDescriptors,
	kEnablingReports,
	kReady
};

// A peripheral may expose several input reports, each with its own value and
// configuration handle.
#define kMaxReports 8

struct att_report {
	uint16	valueHandle;
	uint16	configHandle;
	uint16	referenceHandle;
	uint8	reportId;
	bool	input;
};


struct AttSession : DoublyLinkedListLinkImpl<AttSession> {
	HciConnection*	connection;
	hci_id			hid;
	bdaddr_t		address;

	uint16			mtu;
	att_state		state;

	// The HID service and what was found inside it.
	uint16			serviceStart;
	uint16			serviceEnd;
	uint16			protocolModeHandle;
	uint16			keyboardValueHandle;
	uint16			mouseValueHandle;
	uint16			keyboardConfigHandle;
	uint16			mouseConfigHandle;

	// Where the descriptor search has got to.
	uint16			searchHandle;
	uint16			pendingValueHandle;
	// The value handle of the characteristic a descriptor belongs to, which
	// is whichever declaration was seen most recently.
	uint16			currentValueHandle;

	// Report protocol mode.
	uint16			reportMapHandle;
	uint8			reportMap[BLUETOOTH_HID_MAX_REPORT_MAP];
	uint16			reportMapSize;
	att_report		reports[kMaxReports];
	uint8			reportCount;
	uint8			reportIndex;
	bool			reportMode;

	bool			waitingForEncryption;
};


static DoublyLinkedList<AttSession> sSessions;
static mutex sSessionsLock = MUTEX_INITIALIZER("bluetooth ATT sessions");

static const uint16 kDefaultMtu = 23;
static const uint16 kPreferredMtu = 64;


static AttSession*
find_session(HciConnection* connection)
{
	DoublyLinkedList<AttSession>::Iterator iterator = sSessions.GetIterator();
	while (iterator.HasNext()) {
		AttSession* session = iterator.Next();
		if (session->connection == connection)
			return session;
	}

	return NULL;
}


/*!	Attribute protocol packets carry the opcode and its parameters directly on
	the fixed channel, with nothing but the basic L2CAP header in front.
*/
static status_t
send_att_packet(AttSession* session, uint8 opcode, const void* data,
	size_t size)
{
	// L2CAP header, opcode, and the longest request this client sends.
	uint8 packet[sizeof(l2cap_basic_header) + 1 + 16];
	const size_t total = sizeof(l2cap_basic_header) + 1 + size;

	if (size > sizeof(packet) - sizeof(l2cap_basic_header) - 1)
		return B_BAD_VALUE;

	l2cap_basic_header* header = (l2cap_basic_header*)packet;
	header->length = B_HOST_TO_LENDIAN_INT16(1 + size);
	header->dcid = B_HOST_TO_LENDIAN_INT16(L2CAP_ATT_CID);
	packet[sizeof(l2cap_basic_header)] = opcode;
	if (size > 0)
		memcpy(&packet[sizeof(l2cap_basic_header) + 1], data, size);

	// One prepend into a generous header space, so the packet cannot end up
	// spanning two nodes: the transport driver requires it contiguous.
	net_buffer* buffer = gBufferModule->create(128);
	if (buffer == NULL)
		return B_NO_MEMORY;

	void* payload = NULL;
	status_t status = gBufferModule->prepend_size(buffer, total, &payload);
	if (status != B_OK || payload == NULL) {
		gBufferModule->free(buffer);
		return status != B_OK ? status : B_ERROR;
	}

	memcpy(payload, packet, total);
	buffer->type = session->connection->handle;

	status = btDevices->PostACL(session->hid, buffer);
	if (status != B_OK)
		gBufferModule->free(buffer);

	return status;
}


static status_t
send_read_by_group_type(AttSession* session, uint16 start, uint16 end,
	uint16 type)
{
	struct {
		uint16 start;
		uint16 end;
		uint16 type;
	} _PACKED request;

	request.start = B_HOST_TO_LENDIAN_INT16(start);
	request.end = B_HOST_TO_LENDIAN_INT16(end);
	request.type = B_HOST_TO_LENDIAN_INT16(type);

	return send_att_packet(session, ATT_READ_BY_GROUP_TYPE_REQUEST, &request,
		sizeof(request));
}


static status_t
send_read_by_type(AttSession* session, uint16 start, uint16 end, uint16 type)
{
	struct {
		uint16 start;
		uint16 end;
		uint16 type;
	} _PACKED request;

	request.start = B_HOST_TO_LENDIAN_INT16(start);
	request.end = B_HOST_TO_LENDIAN_INT16(end);
	request.type = B_HOST_TO_LENDIAN_INT16(type);

	return send_att_packet(session, ATT_READ_BY_TYPE_REQUEST, &request,
		sizeof(request));
}


static status_t
send_find_information(AttSession* session, uint16 start, uint16 end)
{
	struct {
		uint16 start;
		uint16 end;
	} _PACKED request;

	request.start = B_HOST_TO_LENDIAN_INT16(start);
	request.end = B_HOST_TO_LENDIAN_INT16(end);

	return send_att_packet(session, ATT_FIND_INFORMATION_REQUEST, &request,
		sizeof(request));
}


static status_t
send_write(AttSession* session, uint16 handle, const void* value, size_t size)
{
	uint8 request[2 + 8];
	if (size > sizeof(request) - 2)
		return B_BAD_VALUE;

	uint16 littleHandle = B_HOST_TO_LENDIAN_INT16(handle);
	memcpy(request, &littleHandle, 2);
	memcpy(request + 2, value, size);

	return send_att_packet(session, ATT_WRITE_REQUEST, request, size + 2);
}


static status_t
send_read_blob(AttSession* session, uint16 handle, uint16 offset)
{
	struct {
		uint16 handle;
		uint16 offset;
	} _PACKED request;

	request.handle = B_HOST_TO_LENDIAN_INT16(handle);
	request.offset = B_HOST_TO_LENDIAN_INT16(offset);

	return send_att_packet(session, ATT_READ_BLOB_REQUEST, &request,
		sizeof(request));
}


static status_t
send_read(AttSession* session, uint16 handle)
{
	uint16 request = B_HOST_TO_LENDIAN_INT16(handle);
	return send_att_packet(session, ATT_READ_REQUEST, &request,
		sizeof(request));
}


/*!	Turns on notifications for one report characteristic by writing its client
	configuration descriptor.
*/
static status_t
enable_notifications(AttSession* session, uint16 configHandle)
{
	uint16 value = B_HOST_TO_LENDIAN_INT16(0x0001);
	return send_write(session, configHandle, &value, sizeof(value));
}


/*!	Walks the state machine on to whatever should be asked for next. */
static void
advance_discovery(AttSession* session)
{
	switch (session->state) {
		case kSettingBootMode:
			// Boot mode fixes the report layout, so nothing has to interpret
			// the report map.
			if (session->keyboardConfigHandle != 0) {
				session->state = kEnablingKeyboard;
				enable_notifications(session, session->keyboardConfigHandle);
				return;
			}
			// fall through

		case kEnablingKeyboard:
			if (session->mouseConfigHandle != 0) {
				session->state = kEnablingMouse;
				enable_notifications(session, session->mouseConfigHandle);
				return;
			}
			// fall through

		case kEnablingMouse:
			session->state = kReady;
			TRACE("%s: HID over GATT ready, keyboard=%#x mouse=%#x\n",
				__func__, session->keyboardValueHandle,
				session->mouseValueHandle);
			return;

		default:
			return;
	}
}


/*!	Hands the assembled report descriptor to the input server add-on, which
	needs it to make sense of anything this peripheral sends.
*/
static void
deliver_report_map(AttSession* session)
{
	port_id port = find_port(BLUETOOTH_HID_PORT_NAME);
	if (port < B_OK)
		return;

	bluetooth_hid_report_map message;
	memset(&message, 0, sizeof(message));
	message.address = session->address;
	message.size = session->reportMapSize;
	memcpy(message.data, session->reportMap, session->reportMapSize);

	write_port_etc(port, BLUETOOTH_HID_REPORT_MAP, &message, sizeof(message),
		B_RELATIVE_TIMEOUT, 0);

	TRACE("%s: report map of %d bytes delivered\n", __func__,
		session->reportMapSize);
}


/*!	Walks the input reports, turning notifications on for each in turn. */
static void
enable_next_report(AttSession* session)
{
	while (session->reportIndex < session->reportCount) {
		att_report& report = session->reports[session->reportIndex];
		if (report.input && report.configHandle != 0) {
			session->state = kEnablingReports;
			enable_notifications(session, report.configHandle);
			return;
		}
		session->reportIndex++;
	}

	session->state = kReady;
	TRACE("%s: HID over GATT ready in report mode, %d report(s)\n", __func__,
		session->reportCount);
}


/*!	Reads the report reference descriptor of each report characteristic, which
	says which report id it carries and whether it is an input.
*/
static void
read_next_report_reference(AttSession* session)
{
	while (session->reportIndex < session->reportCount) {
		att_report& report = session->reports[session->reportIndex];
		if (report.referenceHandle != 0) {
			session->state = kFindingReportDescriptors;
			send_read(session, report.referenceHandle);
			return;
		}
		session->reportIndex++;
	}

	// Everything that could be described has been; subscribe now.
	session->reportIndex = 0;
	enable_next_report(session);
}


/*!	Once both report characteristics are known, ask for boot mode and then
	subscribe to each of them in turn.
*/
static void
finish_characteristic_discovery(AttSession* session)
{
	if (session->keyboardValueHandle == 0 && session->mouseValueHandle == 0) {
		// No boot reports. The device describes its own layout instead, so
		// fetch that description and work from it.
		if (session->reportMapHandle == 0 || session->reportCount == 0) {
			ERROR("%s: device offers neither boot reports nor a report map\n",
				__func__);
			session->state = kIdle;
			return;
		}

		TRACE("%s: no boot mode, falling back to report mode\n", __func__);
		session->reportMode = true;
		session->reportMapSize = 0;
		session->state = kReadingReportMap;
		send_read_blob(session, session->reportMapHandle, 0);
		return;
	}

	if (session->protocolModeHandle != 0) {
		uint8 mode = GATT_PROTOCOL_MODE_BOOT;
		session->state = kSettingBootMode;
		// A write command draws no response, so nothing waits on it.
		uint8 request[3];
		uint16 handle
			= B_HOST_TO_LENDIAN_INT16(session->protocolModeHandle);
		memcpy(request, &handle, 2);
		request[2] = mode;
		send_att_packet(session, ATT_WRITE_COMMAND, request, sizeof(request));
	} else
		session->state = kSettingBootMode;

	advance_discovery(session);
}


/*!	Descriptor discovery is one sweep of the service: every descriptor belongs
	to the characteristic whose declaration came before it.
*/
static void
find_next_descriptor(AttSession* session)
{
	if (session->searchHandle == 0 || session->searchHandle
			> session->serviceEnd) {
		finish_characteristic_discovery(session);
		return;
	}

	session->state = kFindingDescriptors;
	send_find_information(session, session->searchHandle,
		session->serviceEnd);
}


static void
assign_descriptor(AttSession* session, uint16 handle, uint16 uuid)
{
	if (uuid != GATT_CLIENT_CHARACTERISTIC_CONFIGURATION
		&& uuid != GATT_REPORT_REFERENCE) {
		return;
	}

	uint16 owner = session->currentValueHandle;
	if (owner == 0)
		return;

	if (uuid == GATT_CLIENT_CHARACTERISTIC_CONFIGURATION) {
		if (owner == session->keyboardValueHandle)
			session->keyboardConfigHandle = handle;
		else if (owner == session->mouseValueHandle)
			session->mouseConfigHandle = handle;
	}

	for (uint8 i = 0; i < session->reportCount; i++) {
		if (session->reports[i].valueHandle != owner)
			continue;

		if (uuid == GATT_CLIENT_CHARACTERISTIC_CONFIGURATION)
			session->reports[i].configHandle = handle;
		else
			session->reports[i].referenceHandle = handle;
		return;
	}
}


static status_t
handle_read_by_group_type_response(AttSession* session, net_buffer* buffer)
{
	uint8 data[64];
	size_t size = min_c(buffer->size, sizeof(data));
	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return ENOBUFS;

	if (size < 1)
		return EMSGSIZE;

	// The first byte gives the length of each attribute record that follows.
	uint8 recordSize = data[0];
	if (recordSize < 6)
		return EMSGSIZE;

	uint16 lastEnd = 0;
	for (size_t offset = 1; offset + recordSize <= size; offset += recordSize) {
		const uint8* record = &data[offset];
		uint16 start = B_LENDIAN_TO_HOST_INT16(*(const uint16*)record);
		uint16 end = B_LENDIAN_TO_HOST_INT16(*(const uint16*)(record + 2));
		lastEnd = end;

		// Only the sixteen bit form matters here; the HID service is one.
		if (recordSize == 6) {
			uint16 uuid = B_LENDIAN_TO_HOST_INT16(*(const uint16*)(record + 4));
			if (uuid == GATT_HID_SERVICE) {
				session->serviceStart = start;
				session->serviceEnd = end;
				TRACE("%s: HID service at %#x-%#x\n", __func__, start, end);

				// HID over GATT requires the link to be encrypted before a
				// peripheral will report anything. Several will accept the
				// subscription unencrypted and then simply stay silent, so
				// waiting to be refused is not enough: pair as soon as the
				// service is known to be there.
				smp_start_pairing(session->connection);

				session->state = kFindingCharacteristics;
				return send_read_by_type(session, start, end,
					GATT_CHARACTERISTIC);
			}
		}
	}

	if (lastEnd == 0 || lastEnd == 0xffff) {
		ERROR("%s: no HID service on this device\n", __func__);
		session->state = kIdle;
		return B_OK;
	}

	// Keep walking the rest of the attribute range.
	return send_read_by_group_type(session, (uint16)(lastEnd + 1), 0xffff,
		GATT_PRIMARY_SERVICE);
}


static status_t
handle_read_by_type_response(AttSession* session, net_buffer* buffer)
{
	uint8 data[64];
	size_t size = min_c(buffer->size, sizeof(data));
	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return ENOBUFS;

	if (size < 1)
		return EMSGSIZE;

	uint8 recordSize = data[0];
	if (recordSize < 7)
		return EMSGSIZE;

	uint16 lastHandle = 0;
	for (size_t offset = 1; offset + recordSize <= size; offset += recordSize) {
		const uint8* record = &data[offset];
		lastHandle = B_LENDIAN_TO_HOST_INT16(*(const uint16*)record);

		// Declaration: properties, value handle, then the characteristic UUID.
		uint16 valueHandle
			= B_LENDIAN_TO_HOST_INT16(*(const uint16*)(record + 3));

		if (recordSize == 7) {
			uint16 uuid
				= B_LENDIAN_TO_HOST_INT16(*(const uint16*)(record + 5));

			switch (uuid) {
				case GATT_PROTOCOL_MODE:
					session->protocolModeHandle = valueHandle;
					break;
				case GATT_BOOT_KEYBOARD_INPUT_REPORT:
					session->keyboardValueHandle = valueHandle;
					TRACE("%s: boot keyboard report at %#x\n", __func__,
						valueHandle);
					break;
				case GATT_BOOT_MOUSE_INPUT_REPORT:
					session->mouseValueHandle = valueHandle;
					TRACE("%s: boot mouse report at %#x\n", __func__,
						valueHandle);
					break;

				case GATT_REPORT_MAP:
					session->reportMapHandle = valueHandle;
					break;

				case GATT_REPORT:
					if (session->reportCount < kMaxReports) {
						att_report& report
							= session->reports[session->reportCount++];
						memset(&report, 0, sizeof(report));
						report.valueHandle = valueHandle;
						TRACE("%s: report characteristic at %#x\n", __func__,
							valueHandle);
					}
					break;
			}
		}
	}

	if (lastHandle != 0 && lastHandle < session->serviceEnd) {
		return send_read_by_type(session, (uint16)(lastHandle + 1),
			session->serviceEnd, GATT_CHARACTERISTIC);
	}

	// Every characteristic is known; sweep the service for the descriptors
	// that belong to them.
	session->searchHandle = session->serviceStart;
	session->currentValueHandle = 0;
	find_next_descriptor(session);
	return B_OK;
}


static status_t
handle_find_information_response(AttSession* session, net_buffer* buffer)
{
	uint8 data[64];
	size_t size = min_c(buffer->size, sizeof(data));
	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return ENOBUFS;

	if (size < 1)
		return EMSGSIZE;

	// Format one is a sixteen bit UUID, which every descriptor of interest
	// here is. A hundred and twenty eight bit one is skipped over.
	const size_t recordSize = data[0] == 0x01 ? 4 : 18;
	if (data[0] != 0x01 && data[0] != 0x02)
		return EMSGSIZE;

	uint16 lastHandle = 0;
	for (size_t offset = 1; offset + recordSize <= size;
			offset += recordSize) {
		uint16 handle = B_LENDIAN_TO_HOST_INT16(*(const uint16*)&data[offset]);
		lastHandle = handle;

		if (recordSize != 4)
			continue;

		uint16 uuid
			= B_LENDIAN_TO_HOST_INT16(*(const uint16*)&data[offset + 2]);

		if (uuid == GATT_CHARACTERISTIC) {
			// The value always sits immediately after its declaration, and
			// every descriptor until the next declaration belongs to it.
			session->currentValueHandle = (uint16)(handle + 1);
			continue;
		}

		assign_descriptor(session, handle, uuid);
	}

	if (lastHandle != 0 && lastHandle < session->serviceEnd) {
		session->searchHandle = (uint16)(lastHandle + 1);
		find_next_descriptor(session);
		return B_OK;
	}

	session->searchHandle = 0;
	finish_characteristic_discovery(session);
	return B_OK;
}


/*!	Hands one boot report to the input server add-on, which already knows how
	to turn it into key and mouse events.
*/
static void
deliver_report(AttSession* session, uint16 handle, const uint8* data,
	size_t size)
{
	port_id port = find_port(BLUETOOTH_HID_PORT_NAME);
	if (port < B_OK)
		return;

	bluetooth_hid_report report;
	memset(&report, 0, sizeof(report));
	report.address = session->address;

	if (handle == session->keyboardValueHandle)
		report.type = BLUETOOTH_HID_KEYBOARD_REPORT;
	else if (handle == session->mouseValueHandle)
		report.type = BLUETOOTH_HID_MOUSE_REPORT;
	else {
		// Report protocol mode: the layout is whatever the report map
		// described, so the report id is what identifies it.
		bool found = false;
		for (uint8 i = 0; i < session->reportCount; i++) {
			if (session->reports[i].valueHandle != handle)
				continue;

			report.type = BLUETOOTH_HID_GENERIC_REPORT;
			report.reportId = session->reports[i].reportId;
			found = true;
			break;
		}

		if (!found)
			return;
	}

	if (size > BLUETOOTH_HID_MAX_REPORT_SIZE)
		size = BLUETOOTH_HID_MAX_REPORT_SIZE;
	report.size = (uint8)size;
	memcpy(report.data, data, size);

	write_port_etc(port, BLUETOOTH_HID_REPORT, &report, sizeof(report),
		B_RELATIVE_TIMEOUT, 0);
}


static status_t
handle_notification(AttSession* session, net_buffer* buffer)
{
	uint8 data[2 + BLUETOOTH_HID_MAX_REPORT_SIZE];
	size_t size = min_c(buffer->size, sizeof(data));
	if (size < 2)
		return EMSGSIZE;

	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return ENOBUFS;

	uint16 handle = B_LENDIAN_TO_HOST_INT16(*(const uint16*)data);
	deliver_report(session, handle, data + 2, size - 2);

	return B_OK;
}


/*!	Answers both the plain read used for a report reference and the blob reads
	that fetch the report map a packet at a time.
*/
static status_t
handle_read_response(AttSession* session, uint8 opcode, net_buffer* buffer)
{
	uint8 data[64];
	size_t size = min_c(buffer->size, sizeof(data));
	if (size > 0 && gBufferModule->read(buffer, 0, data, size) != B_OK)
		return ENOBUFS;

	if (session->state == kReadingReportMap) {
		size_t space = BLUETOOTH_HID_MAX_REPORT_MAP - session->reportMapSize;
		size_t copied = min_c(size, space);
		memcpy(&session->reportMap[session->reportMapSize], data, copied);
		session->reportMapSize += copied;

		// A short answer means the attribute has been read to its end.
		if (size + 1 >= (size_t)session->mtu && space > copied) {
			return send_read_blob(session, session->reportMapHandle,
				session->reportMapSize);
		}

		deliver_report_map(session);

		session->reportIndex = 0;
		read_next_report_reference(session);
		return B_OK;
	}

	if (session->state == kFindingReportDescriptors) {
		// A report reference is the report id followed by its type.
		if (size >= 2 && session->reportIndex < session->reportCount) {
			att_report& report = session->reports[session->reportIndex];
			report.reportId = data[0];
			report.input = data[1] == GATT_REPORT_TYPE_INPUT;
			TRACE("%s: report %d is %s\n", __func__, report.reportId,
				report.input ? "an input" : "not an input");
		}

		session->reportIndex++;
		read_next_report_reference(session);
		return B_OK;
	}

	return B_OK;
}


static status_t
handle_error_response(AttSession* session, net_buffer* buffer)
{
	uint8 data[4];
	if (buffer->size < 4 || gBufferModule->read(buffer, 0, data, 4) != B_OK)
		return EMSGSIZE;

	uint8 request = data[0];
	uint8 error = data[3];

	TRACE("%s: request %#x failed with %#x\n", __func__, request, error);

	if (error == ATT_ERROR_INSUFFICIENT_AUTHENTICATION
		|| error == ATT_ERROR_INSUFFICIENT_ENCRYPTION
		|| error == ATT_ERROR_INSUFFICIENT_AUTHORIZATION) {
		// This is the moment pairing is actually required, rather than
		// something to do speculatively on every connection. Everything is
		// retried from att_link_encrypted() once the link comes up secured.
		TRACE("%s: encryption needed, pairing\n", __func__);
		session->waitingForEncryption = true;
		smp_start_pairing(session->connection);
		return B_OK;
	}

	if (request == ATT_READ_BLOB_REQUEST || request == ATT_READ_REQUEST) {
		if (session->state == kReadingReportMap) {
			// Whatever arrived is all there is.
			deliver_report_map(session);
			session->reportIndex = 0;
			read_next_report_reference(session);
		} else if (session->state == kFindingReportDescriptors) {
			session->reportIndex++;
			read_next_report_reference(session);
		}
		return B_OK;
	}

	if (request == ATT_WRITE_REQUEST && session->state == kEnablingReports) {
		session->reportIndex++;
		enable_next_report(session);
		return B_OK;
	}

	if (request == ATT_READ_BY_GROUP_TYPE_REQUEST
		|| request == ATT_READ_BY_TYPE_REQUEST
		|| request == ATT_FIND_INFORMATION_REQUEST) {
		// Attribute not found is how each of these sweeps reports that it has
		// reached the end, so it moves discovery on rather than stopping it.
		switch (session->state) {
			case kFindingService:
				ERROR("%s: no HID service on this device\n", __func__);
				session->state = kIdle;
				break;

			case kFindingCharacteristics:
				// Every characteristic has been seen. Start the descriptor
				// sweep from the top of the service, which is where the
				// configuration descriptors are found.
				session->searchHandle = session->serviceStart;
				session->currentValueHandle = 0;
				find_next_descriptor(session);
				break;

			case kFindingDescriptors:
				session->searchHandle = 0;
				finish_characteristic_discovery(session);
				break;

			default:
				break;
		}
	}

	return B_OK;
}


status_t
att_start_discovery(HciConnection* connection)
{
	if (connection == NULL)
		return B_BAD_VALUE;

	MutexLocker locker(sSessionsLock);

	if (find_session(connection) != NULL)
		return B_BUSY;

	AttSession* session = new(std::nothrow) AttSession;
	if (session == NULL)
		return B_NO_MEMORY;

	memset(session, 0, sizeof(AttSession));
	session->connection = connection;
	session->hid = connection->Hid;
	session->address = connection->destination;
	session->mtu = kDefaultMtu;
	session->state = kFindingService;

	sSessions.Add(session);

	TRACE("%s: looking for the HID service on handle %#x\n", __func__,
		connection->handle);

	return send_read_by_group_type(session, 0x0001, 0xffff,
		GATT_PRIMARY_SERVICE);
}


void
att_link_encrypted(HciConnection* connection)
{
	MutexLocker locker(sSessionsLock);

	AttSession* session = find_session(connection);
	if (session == NULL)
		return;

	if (session->state == kReady) {
		// The subscription was written before the link was encrypted, and a
		// peripheral is entitled to discard client configuration written
		// without it - which looks exactly like a mouse that pairs and then
		// never reports anything. Write it again now that the link is
		// secure.
		TRACE("%s: link is encrypted, subscribing again\n", __func__);

		if (session->reportMode) {
			session->reportIndex = 0;
			enable_next_report(session);
		} else if (session->keyboardConfigHandle != 0) {
			session->state = kEnablingKeyboard;
			enable_notifications(session, session->keyboardConfigHandle);
		} else if (session->mouseConfigHandle != 0) {
			session->state = kEnablingMouse;
			enable_notifications(session, session->mouseConfigHandle);
		}

		return;
	}

	if (!session->waitingForEncryption)
		return;

	session->waitingForEncryption = false;

	TRACE("%s: link is encrypted, resuming\n", __func__);

	// Whatever was refused for want of encryption is worth another try.
	switch (session->state) {
		case kFindingService:
			send_read_by_group_type(session, 0x0001, 0xffff,
				GATT_PRIMARY_SERVICE);
			break;

		case kFindingCharacteristics:
			send_read_by_type(session, session->serviceStart,
				session->serviceEnd, GATT_CHARACTERISTIC);
			break;

		case kFindingDescriptors:
			send_find_information(session, session->searchHandle,
				session->serviceEnd);
			break;

		case kEnablingKeyboard:
			enable_notifications(session, session->keyboardConfigHandle);
			break;

		case kEnablingMouse:
			enable_notifications(session, session->mouseConfigHandle);
			break;

		default:
			break;
	}
}


status_t
att_receive(HciConnection* connection, net_buffer* buffer)
{
	if (buffer->size < 1) {
		gBufferModule->free(buffer);
		return EMSGSIZE;
	}

	uint8 opcode;
	{
		NetBufferHeaderReader<uint8> code(buffer);
		if (code.Status() != B_OK) {
			gBufferModule->free(buffer);
			return ENOBUFS;
		}
		opcode = *code;
		code.Remove();
	}

	MutexLocker locker(sSessionsLock);
	AttSession* session = find_session(connection);
	if (session == NULL) {
		gBufferModule->free(buffer);
		return B_OK;
	}

	status_t status = B_OK;

	switch (opcode) {
		case ATT_ERROR_RESPONSE:
			status = handle_error_response(session, buffer);
			break;

		case ATT_READ_BY_GROUP_TYPE_RESPONSE:
			status = handle_read_by_group_type_response(session, buffer);
			break;

		case ATT_READ_BY_TYPE_RESPONSE:
			status = handle_read_by_type_response(session, buffer);
			break;

		case ATT_FIND_INFORMATION_RESPONSE:
			status = handle_find_information_response(session, buffer);
			break;

		case ATT_READ_BLOB_RESPONSE:
		case ATT_READ_RESPONSE:
			status = handle_read_response(session, opcode, buffer);
			break;

		case ATT_WRITE_RESPONSE:
			if (session->state == kEnablingReports) {
				session->reportIndex++;
				enable_next_report(session);
			} else
				advance_discovery(session);
			break;

		case ATT_HANDLE_VALUE_NOTIFICATION:
			status = handle_notification(session, buffer);
			break;

		case ATT_HANDLE_VALUE_INDICATION:
			status = handle_notification(session, buffer);
			send_att_packet(session, ATT_HANDLE_VALUE_CONFIRMATION, NULL, 0);
			break;

		default:
			TRACE("%s: unhandled attribute opcode %#x\n", __func__, opcode);
			break;
	}

	gBufferModule->free(buffer);
	return status;
}


void
att_connection_closed(HciConnection* connection)
{
	MutexLocker locker(sSessionsLock);

	AttSession* session = find_session(connection);
	if (session == NULL)
		return;

	// Let the input server release anything still held down.
	port_id port = find_port(BLUETOOTH_HID_PORT_NAME);
	if (port >= B_OK) {
		bluetooth_hid_disconnected message;
		message.address = session->address;
		write_port_etc(port, BLUETOOTH_HID_DISCONNECTED, &message,
			sizeof(message), B_RELATIVE_TIMEOUT, 0);
	}

	sSessions.Remove(session);
	delete session;
}
