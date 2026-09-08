/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <util/DoublyLinkedList.h>
#include <util/AutoLock.h>

#include <net_protocol.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <btDebug.h>
#include <btModules.h>

#include <l2cap.h>

#include "ConnectionInterface.h"

struct net_protocol_module_info* L2cap = NULL;
extern net_buffer_module_info* gBufferModule;


HciConnection::HciConnection(hci_id hid)
{
	mutex_init(&fLock, "HciConnection");
	Hid = hid;
	ndevice = NULL;
	destination = BDADDR_NULL;
	fNextIdent = L2CAP_NULL_IDENT;
	destination_type = 0;
	low_energy = false;
	handle = 0;
	type = 0;
	mtu = L2CAP_MTU_MINIMUM;
	status = HCI_CONN_CLOSED;
	currentRxPacket = NULL;
	currentRxExpectedLength = 0;
	disconnect_hook = NULL;

	// TODO: This doesn't really belong here...
	interface_address = {};
	address_dl = {};
	interface_address.local = (struct sockaddr*)&address_dl;
	interface_address.destination = (struct sockaddr*)&address_dest;
	address_dl.sdl_index = Hid;
}


HciConnection::~HciConnection()
{
	mutex_destroy(&fLock);
}


HciConnection*
AddConnection(uint16 handle, int type, const bdaddr_t& dst, hci_id hid)
{
	// Create connection descriptor

	HciConnection* conn = ConnectionByHandle(handle, hid);
	bool newConnection = conn == NULL;
	if (newConnection) {
		conn = new (std::nothrow) HciConnection(hid);
		if (conn == NULL)
			return NULL;
	}

	// fill values
	bdaddrUtils::Copy(conn->destination, dst);
	{
	sockaddr_l2cap* destination = (sockaddr_l2cap*)&conn->address_dest;
	destination->l2cap_len = sizeof(sockaddr_l2cap);
	destination->l2cap_family = AF_BLUETOOTH;
	destination->l2cap_bdaddr = dst;
	}
	conn->type = type;
	conn->handle = handle;
	conn->status = HCI_CONN_OPEN;
	conn->mtu = L2CAP_MTU_MINIMUM; // TODO: give the mtu to the connection

	if (newConnection) {
		MutexLocker _(&sConnectionListLock);
		sConnectionList.Add(conn);
	}

	return conn;
}


status_t
RemoveConnection(const bdaddr_t& destination, hci_id hid)
{
	HciConnection*	conn = ConnectionByDestination(destination, hid);

	if (conn == NULL)
		return B_ERROR;

	// if the device is still part of the list, remove it
	if (conn->GetDoublyLinkedListLink()->next != NULL
		|| conn->GetDoublyLinkedListLink()->previous != NULL
		|| conn == sConnectionList.Head()) {
		DisconnectL2capEndpoints(conn);
		if (conn->disconnect_hook != NULL)
			conn->disconnect_hook(conn);

		MutexLocker locker(&sConnectionListLock);
		sConnectionList.Remove(conn);
		locker.Unlock();

		delete conn;
		return B_OK;
	}
	return B_ERROR;
}


status_t
RemoveConnection(uint16 handle, hci_id hid)
{
	HciConnection*	conn = ConnectionByHandle(handle, hid);

	if (conn == NULL)
		return B_ERROR;

	// if the device is still part of the list, remove it
	if (conn->GetDoublyLinkedListLink()->next != NULL
		|| conn->GetDoublyLinkedListLink()->previous != NULL
		|| conn == sConnectionList.Head()) {
		DisconnectL2capEndpoints(conn);
		if (conn->disconnect_hook != NULL)
			conn->disconnect_hook(conn);

		MutexLocker locker(&sConnectionListLock);
		sConnectionList.Remove(conn);
		locker.Unlock();

		delete conn;
		return B_OK;
	}

	return B_ERROR;
}


void
RemoveConnections(hci_id hid)
{
	for (;;) {
		HciConnection* connection = NULL;
		{
			MutexLocker locker(&sConnectionListLock);
			auto iterator = sConnectionList.GetIterator();
			while (iterator.HasNext()) {
				HciConnection* candidate = iterator.Next();
				if (candidate->Hid == hid) {
					connection = candidate;
					sConnectionList.Remove(connection);
					break;
				}
			}
		}

		if (connection == NULL)
			return;

		// Endpoints must stop using the connection before its HCI device can
		// disappear.  In particular, the L2CAP send timer may still be active.
		DisconnectL2capEndpoints(connection);
		if (connection->disconnect_hook != NULL)
			connection->disconnect_hook(connection);
		delete connection;
	}
}


status_t
DisconnectL2capEndpoints(HciConnection* conn)
{
	status_t status = B_OK;
	if (L2cap == NULL)
		status = get_module(NET_BLUETOOTH_L2CAP_NAME, (module_info**)&L2cap);
	if (status != B_OK) {
		ERROR("%s: cannot get module \"%s\"\n", __func__,
			NET_BLUETOOTH_L2CAP_NAME);
		return status;
	} // TODO: someone put it

	// Inform the L2CAP module this connection is about to be gone.
	net_buffer* error = gBufferModule->create(128);
	sockaddr_l2cap source = {};
	source.l2cap_bdaddr = conn->destination;
	error->source = (struct sockaddr*)&source;
	error->interface_address = &conn->interface_address;
	status = L2cap->error_received(B_NET_ERROR_UNREACH_HOST, NULL, error);
	if (status != B_OK) {
		error->interface_address = NULL;
		gBufferModule->free(error);
		return status;
	}

	return status;
}


hci_id
RouteConnection(const bdaddr_t& destination)
{
	MutexLocker _(&sConnectionListLock);
	HciConnection* conn;

	DoublyLinkedList<HciConnection>::Iterator iterator
		= sConnectionList.GetIterator();
	while (iterator.HasNext()) {

		conn = iterator.Next();
		if (bdaddrUtils::Compare(conn->destination, destination)) {
			return conn->Hid;
		}
	}

	return -1;
}


HciConnection*
ConnectionByHandle(uint16 handle, hci_id hid)
{
	MutexLocker _(&sConnectionListLock);
	HciConnection*	conn;

	DoublyLinkedList<HciConnection>::Iterator iterator
		= sConnectionList.GetIterator();
	while (iterator.HasNext()) {

		conn = iterator.Next();
		if (conn->Hid == hid && conn->handle == handle) {
			return conn;
		}
	}

	return NULL;
}


HciConnection*
ConnectionByDestination(const bdaddr_t& destination, hci_id hid)
{
	MutexLocker _(&sConnectionListLock);

	DoublyLinkedList<HciConnection>::Iterator iterator
		= sConnectionList.GetIterator();
	while (iterator.HasNext()) {

		HciConnection* conn = iterator.Next();
		if (conn->Hid == hid
			&& bdaddrUtils::Compare(conn->destination, destination)) {
			return conn;
		}
	}

	return NULL;
}


uint8
allocate_command_ident(HciConnection* conn, void* pointer)
{
	MutexLocker _(&conn->fLock);

	uint8 ident = conn->fNextIdent + 1;

	if (ident < L2CAP_FIRST_IDENT)
		ident = L2CAP_FIRST_IDENT;

	while (ident != conn->fNextIdent) {
		if (conn->fInUseIdents.Find(ident) == conn->fInUseIdents.End()) {
			conn->fNextIdent = ident;
			conn->fInUseIdents.Insert(ident, pointer);
			return ident;
		}

		ident++;
		if (ident < L2CAP_FIRST_IDENT)
			ident = L2CAP_FIRST_IDENT;
	}

	return L2CAP_NULL_IDENT;
}


void*
lookup_command_ident(HciConnection* conn, uint8 ident)
{
	MutexLocker _(&conn->fLock);

	auto iter = conn->fInUseIdents.Find(ident);
	if (iter == conn->fInUseIdents.End())
		return NULL;

	return iter->Value();
}


void
free_command_ident(HciConnection* conn, uint8 ident)
{
	MutexLocker _(&conn->fLock);
	conn->fInUseIdents.Remove(ident);
}
