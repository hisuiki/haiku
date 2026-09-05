/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Security Manager, Low Energy legacy pairing in the central role.

	A Low Energy input device will not report anything until the link is
	encrypted, so pairing is not optional the way it is for a classic device.
	Only legacy pairing is implemented: Secure Connections would additionally
	need elliptic curve arithmetic, and every device that supports it also
	supports this.
*/

#include "smp.h"
#include "att.h"

#include "l2cap_internal.h"
#include "smp_crypto.h"

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/l2cap.h>
#include <btCoreData.h>
#include <btDebug.h>

#include <NetBufferUtilities.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/Random.h>

#include <FindDirectory.h>
#include <StorageDefs.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>


struct SmpSession : DoublyLinkedListLinkImpl<SmpSession> {
	HciConnection*	connection;
	uint16			handle;
	hci_id			hid;

	// The pairing request and response are fed back into the confirm value,
	// so both are kept exactly as they went over the air.
	uint8			request[7];
	uint8			response[7];

	uint8			temporaryKey[16];
	uint8			localRandom[16];
	uint8			remoteRandom[16];
	uint8			remoteConfirm[16];

	uint8			localAddress[6];
	uint8			localAddressType;
	uint8			remoteAddress[6];
	uint8			remoteAddressType;

	bool			confirmReceived;

	bdaddr_t remoteAddressAsBdaddr() const
	{
		bdaddr_t address;
		memcpy(&address, remoteAddress, sizeof(address));
		return address;
	}
};


/*!	What a completed pairing leaves behind. Keeping it means a peripheral that
	has been paired once is simply re-encrypted on its next connection, which
	is what stops a mouse asking to be paired every time it wakes up.
*/
struct smp_bond {
	bdaddr_t	address;
	uint8		addressType;
	uint8		key[16];
	uint8		random[8];
	uint16		diversifier;
};

#define SMP_MAX_BONDS 16
#define SMP_BOND_FILE_MAGIC 'HBLE'
#define SMP_BOND_FILE_VERSION 1

struct smp_bond_file {
	uint32		magic;
	uint32		version;
	uint32		count;
	smp_bond	bonds[SMP_MAX_BONDS];
};


static DoublyLinkedList<SmpSession> sSessions;
static mutex sSessionsLock = MUTEX_INITIALIZER("bluetooth SMP sessions");

static smp_bond sBonds[SMP_MAX_BONDS];
static uint32 sBondCount = 0;
static bool sBondsLoaded = false;


static bool
same_address(const bdaddr_t& one, const bdaddr_t& other)
{
	return memcmp(&one, &other, sizeof(bdaddr_t)) == 0;
}


static status_t
bond_file_path(char* path, size_t size)
{
	if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, -1, false, path, size)
			!= B_OK) {
		return B_ERROR;
	}

	strlcat(path, "/bluetooth_le_bonds", size);
	return B_OK;
}


/*!	Read once, lazily: at module load time the boot volume is not necessarily
	mounted yet.
*/
static void
load_bonds()
{
	if (sBondsLoaded)
		return;

	sBondsLoaded = true;

	char path[B_PATH_NAME_LENGTH];
	if (bond_file_path(path, sizeof(path)) != B_OK)
		return;

	int file = open(path, B_READ_ONLY);
	if (file < 0)
		return;

	smp_bond_file contents;
	ssize_t bytes = read(file, &contents, sizeof(contents));
	close(file);

	if (bytes != (ssize_t)sizeof(contents)
		|| contents.magic != SMP_BOND_FILE_MAGIC
		|| contents.version != SMP_BOND_FILE_VERSION
		|| contents.count > SMP_MAX_BONDS) {
		return;
	}

	sBondCount = contents.count;
	memcpy(sBonds, contents.bonds, sizeof(sBonds));

	TRACE("%s: %" B_PRIu32 " bond(s) restored\n", __func__, sBondCount);
}


static void
save_bonds()
{
	char path[B_PATH_NAME_LENGTH];
	if (bond_file_path(path, sizeof(path)) != B_OK)
		return;

	smp_bond_file contents;
	memset(&contents, 0, sizeof(contents));
	contents.magic = SMP_BOND_FILE_MAGIC;
	contents.version = SMP_BOND_FILE_VERSION;
	contents.count = sBondCount;
	memcpy(contents.bonds, sBonds, sizeof(sBonds));

	int file = open(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE, 0600);
	if (file < 0) {
		TRACE("%s: could not store bonds\n", __func__);
		return;
	}

	write(file, &contents, sizeof(contents));
	close(file);
}


static smp_bond*
find_bond(const bdaddr_t& address)
{
	load_bonds();

	for (uint32 i = 0; i < sBondCount; i++) {
		if (same_address(sBonds[i].address, address))
			return &sBonds[i];
	}

	return NULL;
}


static smp_bond*
add_bond(const bdaddr_t& address, uint8 addressType)
{
	load_bonds();

	smp_bond* bond = find_bond(address);
	if (bond != NULL)
		return bond;

	if (sBondCount < SMP_MAX_BONDS) {
		bond = &sBonds[sBondCount++];
	} else {
		// Full, so the oldest gives way.
		memmove(&sBonds[0], &sBonds[1], sizeof(smp_bond) * (SMP_MAX_BONDS - 1));
		bond = &sBonds[SMP_MAX_BONDS - 1];
	}

	memset(bond, 0, sizeof(smp_bond));
	bond->address = address;
	bond->addressType = addressType;

	return bond;
}


static SmpSession*
find_session(HciConnection* connection)
{
	DoublyLinkedList<SmpSession>::Iterator iterator = sSessions.GetIterator();
	while (iterator.HasNext()) {
		SmpSession* session = iterator.Next();
		if (session->connection == connection)
			return session;
	}

	return NULL;
}


static void
remove_session(SmpSession* session)
{
	sSessions.Remove(session);
	// It holds key material, so do not leave it lying in freed memory.
	memset(session, 0, sizeof(SmpSession));
	delete session;
}


/*!	Sends one Security Manager packet. These are not signalling commands: the
	channel carries the opcode and its parameters directly, with nothing but
	the basic L2CAP header in front.
*/
static status_t
send_smp_packet(HciConnection* connection, uint8 code, const void* data,
	size_t size)
{
	// L2CAP header, opcode, and the largest payload the Security Manager
	// sends, which is a sixteen byte key or confirm value.
	uint8 packet[sizeof(l2cap_basic_header) + 1 + 16];
	const size_t total = sizeof(l2cap_basic_header) + 1 + size;

	if (size > sizeof(packet) - sizeof(l2cap_basic_header) - 1)
		return B_BAD_VALUE;

	l2cap_basic_header* header = (l2cap_basic_header*)packet;
	header->length = B_HOST_TO_LENDIAN_INT16(1 + size);
	header->dcid = B_HOST_TO_LENDIAN_INT16(L2CAP_SMP_CID);
	packet[sizeof(l2cap_basic_header)] = code;
	if (size > 0)
		memcpy(&packet[sizeof(l2cap_basic_header) + 1], data, size);

	// Generous header space, so this prepend and the ACL header the transport
	// adds later both stay inside the first node.
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
	buffer->type = connection->handle;

	status = btDevices->PostACL(connection->Hid, buffer);
	if (status != B_OK)
		gBufferModule->free(buffer);

	return status;
}


static status_t
send_pairing_failed(HciConnection* connection, uint8 reason)
{
	return send_smp_packet(connection, SMP_PAIRING_FAILED, &reason, 1);
}


/*!	Turns on encryption with the short term key just derived. A legacy pairing
	uses a zero diversifier and a zero random value for this first encryption;
	the long term key the peer distributes afterwards is what later
	reconnections use.
*/
static status_t
start_encryption_with(SmpSession* session, const uint8 key[16],
	const uint8 random[8], uint16 diversifier)
{
	struct hci_command_header header;
	struct hci_cp_le_start_encryption parameters;

	memset(&parameters, 0, sizeof(parameters));
	parameters.handle = B_HOST_TO_LENDIAN_INT16(session->handle);
	parameters.diversifier = B_HOST_TO_LENDIAN_INT16(diversifier);
	if (random != NULL)
		memcpy(parameters.random, random, 8);
	memcpy(parameters.key, key, 16);

	header.opcode = B_HOST_TO_LENDIAN_INT16(
		PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_START_ENCRYPTION));
	header.clen = sizeof(parameters);

	uint8 packet[sizeof(header) + sizeof(parameters)];
	memcpy(packet, &header, sizeof(header));
	memcpy(packet + sizeof(header), &parameters, sizeof(parameters));

	net_buffer* buffer = gBufferModule->create(128);
	if (buffer == NULL)
		return B_NO_MEMORY;

	void* payload = NULL;
	status_t status = gBufferModule->prepend_size(buffer, sizeof(packet),
		&payload);
	if (status != B_OK || payload == NULL) {
		gBufferModule->free(buffer);
		return status != B_OK ? status : B_ERROR;
	}

	memcpy(payload, packet, sizeof(packet));

	TRACE("%s: starting encryption on handle %#x\n", __func__,
		session->handle);

	status = btDevices->PostCommand(session->hid, buffer);
	if (status != B_OK)
		gBufferModule->free(buffer);

	return status;
}


void
smp_link_established(HciConnection* connection)
{
	if (connection == NULL)
		return;

	MutexLocker locker(sSessionsLock);

	smp_bond* bond = find_bond(connection->destination);
	if (bond == NULL) {
		// HID over GATT always ends up needing an encrypted link, and a
		// peripheral that believes it is already bonded drops a connection
		// that neither encrypts nor offers to pair. So offer straight away
		// rather than waiting to be refused.
		locker.Unlock();
		smp_start_pairing(connection);
		return;
	}

	SmpSession session;
	memset(&session, 0, sizeof(session));
	session.connection = connection;
	session.handle = connection->handle;
	session.hid = connection->Hid;

	TRACE("%s: resuming the bond on handle %#x\n", __func__,
		connection->handle);

	start_encryption_with(&session, bond->key, bond->random,
		bond->diversifier);
}


status_t
smp_start_pairing(HciConnection* connection)
{
	if (connection == NULL || connection->ndevice == NULL)
		return B_BAD_VALUE;

	MutexLocker locker(sSessionsLock);

	if (find_session(connection) != NULL) {
		TRACE("%s: pairing already under way\n", __func__);
		return B_BUSY;
	}

	// An existing bond has already been handed to the controller, so the link
	// is on its way to being encrypted without pairing again.
	if (find_bond(connection->destination) != NULL) {
		TRACE("%s: already bonded with this peer\n", __func__);
		return B_OK;
	}

	SmpSession* session = new(std::nothrow) SmpSession;
	if (session == NULL)
		return B_NO_MEMORY;

	memset(session, 0, sizeof(SmpSession));
	session->connection = connection;
	session->handle = connection->handle;
	session->hid = connection->Hid;

	// We connected, so we are the initiator, from our own public address.
	memcpy(session->localAddress, &connection->ndevice->localAddress, 6);
	session->localAddressType = LE_PUBLIC_ADDRESS;
	memcpy(session->remoteAddress, &connection->destination, 6);
	session->remoteAddressType = connection->destination_type;

	// Just Works: claiming no input and no output rules out every method that
	// would need a passkey the user cannot enter anyway.
	session->request[0] = SMP_PAIRING_REQUEST;
	session->request[1] = SMP_IO_NO_INPUT_NO_OUTPUT;
	session->request[2] = 0x00;					// no out of band data
	session->request[3] = SMP_AUTH_BONDING;		// bond, no MITM, legacy
	session->request[4] = SMP_MAX_ENCRYPTION_KEY_SIZE;
	session->request[5] = 0x00;					// we distribute nothing
	// Ask for the long term key, so reconnecting does not pair again.
	session->request[6] = SMP_DIST_ENC_KEY | SMP_DIST_ID_KEY;

	sSessions.Add(session);

	TRACE("%s: requesting pairing on handle %#x\n", __func__,
		connection->handle);

	return send_smp_packet(connection, SMP_PAIRING_REQUEST,
		&session->request[1], 6);
}


static status_t
handle_pairing_response(SmpSession* session, net_buffer* buffer)
{
	if (buffer->size < 6)
		return EMSGSIZE;

	session->response[0] = SMP_PAIRING_RESPONSE;
	if (gBufferModule->read(buffer, 0, &session->response[1], 6) != B_OK)
		return ENOBUFS;

	TRACE("%s: io=%d oob=%d auth=%#x keysize=%d init=%#x resp=%#x\n",
		__func__, session->response[1], session->response[2],
		session->response[3], session->response[4], session->response[5],
		session->response[6]);

	if (session->response[4] < 7) {
		// Refusing a key shorter than the spec's own minimum.
		send_pairing_failed(session->connection, 0x06);
		return B_NOT_ALLOWED;
	}

	// Just Works leaves the temporary key all zeroes.
	memset(session->temporaryKey, 0, sizeof(session->temporaryKey));

	for (int i = 0; i < 16; i += 4) {
		uint32 value = secure_get_random<uint32>();
		memcpy(&session->localRandom[i], &value, sizeof(value));
	}

	uint8 confirm[16];
	smp_c1(session->temporaryKey, session->localRandom, session->request,
		session->response, session->localAddressType, session->localAddress,
		session->remoteAddressType, session->remoteAddress, confirm);

	return send_smp_packet(session->connection, SMP_PAIRING_CONFIRM, confirm,
		sizeof(confirm));
}


static status_t
handle_pairing_confirm(SmpSession* session, net_buffer* buffer)
{
	if (buffer->size < 16)
		return EMSGSIZE;

	if (gBufferModule->read(buffer, 0, session->remoteConfirm, 16) != B_OK)
		return ENOBUFS;

	session->confirmReceived = true;

	// Our own random value can be revealed now that the peer has committed to
	// its confirm value.
	return send_smp_packet(session->connection, SMP_PAIRING_RANDOM,
		session->localRandom, sizeof(session->localRandom));
}


static status_t
handle_pairing_random(SmpSession* session, net_buffer* buffer)
{
	if (buffer->size < 16)
		return EMSGSIZE;

	if (!session->confirmReceived) {
		send_pairing_failed(session->connection, 0x08);
		return B_NOT_ALLOWED;
	}

	if (gBufferModule->read(buffer, 0, session->remoteRandom, 16) != B_OK)
		return ENOBUFS;

	// The peer committed to this random value earlier; check that it did not
	// change its mind after seeing ours.
	uint8 expected[16];
	smp_c1(session->temporaryKey, session->remoteRandom, session->request,
		session->response, session->localAddressType, session->localAddress,
		session->remoteAddressType, session->remoteAddress, expected);

	if (memcmp(expected, session->remoteConfirm, 16) != 0) {
		ERROR("%s: confirm value mismatch, pairing aborted\n", __func__);
		send_pairing_failed(session->connection, 0x04);
		return B_NOT_ALLOWED;
	}

	uint8 shortTermKey[16];
	smp_s1(session->temporaryKey, session->remoteRandom, session->localRandom,
		shortTermKey);

	// A freshly paired link is encrypted with a zero diversifier and random
	// value; only a stored bond names its key with those.
	status_t status = start_encryption_with(session, shortTermKey, NULL, 0);
	memset(shortTermKey, 0, sizeof(shortTermKey));

	// Reads and writes the peripheral refused for want of encryption can be
	// tried again now.
	att_link_encrypted(session->connection);

	return status;
}


status_t
smp_receive(HciConnection* connection, net_buffer* buffer)
{
	if (buffer->size < 1) {
		gBufferModule->free(buffer);
		return EMSGSIZE;
	}

	uint8 code;
	{
		NetBufferHeaderReader<uint8> opcode(buffer);
		if (opcode.Status() != B_OK) {
			gBufferModule->free(buffer);
			return ENOBUFS;
		}
		code = *opcode;
		opcode.Remove();
	}

	MutexLocker locker(sSessionsLock);
	SmpSession* session = find_session(connection);

	TRACE("%s: code=%#x size=%" B_PRIu32 "%s\n", __func__, code, buffer->size,
		session == NULL ? " (no session)" : "");

	status_t status = B_OK;

	switch (code) {
		case SMP_SECURITY_REQUEST:
			// The peer wants the link secured. Starting a pairing is our job
			// as the central.
			locker.Unlock();
			gBufferModule->free(buffer);
			return smp_start_pairing(connection);

		case SMP_PAIRING_RESPONSE:
			if (session == NULL)
				break;
			status = handle_pairing_response(session, buffer);
			break;

		case SMP_PAIRING_CONFIRM:
			if (session == NULL)
				break;
			status = handle_pairing_confirm(session, buffer);
			break;

		case SMP_PAIRING_RANDOM:
			if (session == NULL)
				break;
			status = handle_pairing_random(session, buffer);
			break;

		case SMP_PAIRING_FAILED:
		{
			uint8 reason = 0;
			gBufferModule->read(buffer, 0, &reason, 1);
			ERROR("%s: peer refused pairing, reason %#x\n", __func__, reason);
			if (session != NULL)
				remove_session(session);
			break;
		}

		case SMP_ENCRYPTION_INFORMATION:
		{
			// The long term key, distributed once encryption is up.
			if (buffer->size < 16 || session == NULL)
				break;

			smp_bond* bond = add_bond(session->remoteAddressAsBdaddr(),
				session->remoteAddressType);
			if (bond != NULL)
				gBufferModule->read(buffer, 0, bond->key, 16);

			att_link_encrypted(connection);
			break;
		}

		case SMP_CENTRAL_IDENTIFICATION:
		{
			// The diversifier and random value that name the key above.
			if (buffer->size < 10 || session == NULL)
				break;

			smp_bond* bond = add_bond(session->remoteAddressAsBdaddr(),
				session->remoteAddressType);
			if (bond != NULL) {
				uint8 data[10];
				if (gBufferModule->read(buffer, 0, data, 10) == B_OK) {
					bond->diversifier
						= B_LENDIAN_TO_HOST_INT16(*(uint16*)data);
					memcpy(bond->random, data + 2, 8);
					save_bonds();
					TRACE("%s: bonded, pairing will be skipped next time\n",
						__func__);
				}
			}

			att_link_encrypted(connection);
			break;
		}

		case SMP_IDENTITY_INFORMATION:
		case SMP_IDENTITY_ADDRESS_INFORMATION:
		case SMP_SIGNING_INFORMATION:
			TRACE("%s: key distribution packet %#x\n", __func__, code);
			// These only ever arrive over an encrypted link, so this is a
			// dependable second chance for anything still waiting on one.
			att_link_encrypted(connection);
			break;

		default:
			TRACE("%s: unhandled Security Manager code %#x\n", __func__, code);
			break;
	}

	gBufferModule->free(buffer);
	return status;
}


/*!	Called through HciConnection::disconnect_hook when the link goes away. */
void
smp_connection_closed(HciConnection* connection)
{
	MutexLocker locker(sSessionsLock);

	SmpSession* session = find_session(connection);
	if (session != NULL)
		remove_session(session);
}
