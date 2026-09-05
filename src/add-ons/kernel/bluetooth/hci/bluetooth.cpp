/*
 * Copyright 2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 */

#include <new>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <lock.h>
#include <SupportDefs.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <net_buffer.h>
#include <net_device.h>
#include <net_stack.h>
#include <NetBufferUtilities.h>

#include <btDebug.h>
#include <btCoreData.h>
#include <btModules.h>
#include <CodeHandler.h>
#define KERNEL_LAND
#include <PortListener.h>
#undef KERNEL_LAND

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_acl.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_transport.h>
#include <bluetooth/HCI/btHCI_sco.h>
#include <bluetooth/bdaddrUtils.h>

#include "acl.h"


int32 api_version = B_CUR_DRIVER_API_VERSION;


typedef PortListener<void,
	HCI_MAX_FRAME_SIZE, // Event Body can hold max 255 + 2 header
	24					// Some devices have sent chunks of 24 events(inquiry result)
	> BluetoothRawDataPort;


// Modules references
net_buffer_module_info* gBufferModule = NULL;
struct bluetooth_core_data_module_info* btCoreData = NULL;

static mutex sListLock;
static sem_id sLinkChangeSemaphore;
static DoublyLinkedList<bluetooth_device> sDeviceList;

BluetoothRawDataPort* BluetoothRXPort;

// forward declarations
status_t HciPacketHandler(void* data, int32 code, size_t size);


bluetooth_device*
FindDeviceByID(hci_id hid)
{
	bluetooth_device* device;

	DoublyLinkedList<bluetooth_device>::Iterator iterator
		= sDeviceList.GetIterator();

	while (iterator.HasNext()) {
		device = iterator.Next();
		if (device->index == hid)
			return device;
	}

	return NULL;
}


status_t
PostTransportPacket(hci_id hid, bt_packet_t type, void* data, size_t count)
{
	uint32 code = 0;

	Bluetooth::CodeHandler::SetDevice(&code, hid);
	Bluetooth::CodeHandler::SetProtocol(&code, type);

	return BluetoothRXPort->Trigger(code, data, count);
}


status_t
Assemble(bluetooth_device* bluetoothDevice, bt_packet_t type, void* data,
	size_t count)
{
	if (type != BT_EVENT && type != BT_ACL)
		return B_NOT_SUPPORTED;

	const size_t headerSize = type == BT_EVENT
		? HCI_EVENT_HDR_SIZE : HCI_ACL_HDR_SIZE;
	uint8* cursor = (uint8*)data;

	while (count > 0) {
		net_buffer* nbuf = bluetoothDevice->fBuffersRx[type];

		// Fast path. USB interrupt transfers may contain more than one event,
		// so consume exactly one packet and keep parsing the remainder.
		if (nbuf == NULL && count >= headerSize) {
			size_t packetSize;
			if (type == BT_EVENT) {
				hci_event_header* header = (hci_event_header*)cursor;
				packetSize = HCI_EVENT_HDR_SIZE + header->elen;
			} else {
				hci_acl_header* header = (hci_acl_header*)cursor;
				packetSize = HCI_ACL_HDR_SIZE
					+ B_LENDIAN_TO_HOST_INT16(header->alen);
			}

			if (count >= packetSize && type == BT_EVENT) {
				btCoreData->PostEvent(bluetoothDevice, cursor, packetSize);
				cursor += packetSize;
				count -= packetSize;
				continue;
			}

			bluetoothDevice->fExpectedPacketSize[type] = packetSize;
		}

		if (nbuf == NULL) {
			nbuf = gBufferModule->create(0);
			if (nbuf == NULL)
				return B_NO_MEMORY;
			nbuf->protocol = type;
			bluetoothDevice->fBuffersRx[type] = nbuf;
		}

		// A USB transfer is allowed to split even the HCI header. Accumulate it
		// first, then derive the complete packet length.
		if (bluetoothDevice->fExpectedPacketSize[type] == 0) {
			size_t bytes = min_c(count, headerSize - nbuf->size);
			status_t status = gBufferModule->append(nbuf, cursor, bytes);
			if (status != B_OK)
				return status;
			cursor += bytes;
			count -= bytes;
			if (nbuf->size < headerSize)
				continue;

			uint8 headerBytes[HCI_ACL_HDR_SIZE];
			status = gBufferModule->read(nbuf, 0, headerBytes, headerSize);
			if (status != B_OK)
				return status;
			if (type == BT_EVENT) {
				hci_event_header* header = (hci_event_header*)headerBytes;
				bluetoothDevice->fExpectedPacketSize[type]
					= HCI_EVENT_HDR_SIZE + header->elen;
			} else {
				hci_acl_header* header = (hci_acl_header*)headerBytes;
				bluetoothDevice->fExpectedPacketSize[type] = HCI_ACL_HDR_SIZE
					+ B_LENDIAN_TO_HOST_INT16(header->alen);
			}
		}

		size_t remaining = bluetoothDevice->fExpectedPacketSize[type] - nbuf->size;
		size_t bytes = min_c(count, remaining);
		status_t status = gBufferModule->append(nbuf, cursor, bytes);
		if (status != B_OK)
			return status;
		cursor += bytes;
		count -= bytes;

		if (nbuf->size != bluetoothDevice->fExpectedPacketSize[type])
			continue;

		bluetoothDevice->fBuffersRx[type] = NULL;
		bluetoothDevice->fExpectedPacketSize[type] = 0;
		if (type == BT_EVENT) {
			uint8 event[HCI_MAX_EVENT_SIZE];
			if (nbuf->size > sizeof(event)
				|| gBufferModule->read(nbuf, 0, event, nbuf->size) != B_OK) {
				gBufferModule->free(nbuf);
				return EILSEQ;
			}
			btCoreData->PostEvent(bluetoothDevice, event, nbuf->size);
			gBufferModule->free(nbuf);
		} else
			AclAssembly(nbuf, bluetoothDevice->index);
	}

	return B_OK;
}


status_t
HciPacketHandler(void* data, int32 code, size_t size)
{
	hci_id deviceId = Bluetooth::CodeHandler::Device(code);

	bluetooth_device* bluetoothDevice = FindDeviceByID(deviceId);

	if (bluetoothDevice != NULL) {
		return Assemble(bluetoothDevice, Bluetooth::CodeHandler::Protocol(code),
			data, size);
	} else {
		ERROR("%s: Device 0x%" B_PRIx32 " could not be matched\n", __func__,
			deviceId);
	}

	return B_ERROR;
}


//	#pragma mark -


status_t
RegisterDriver(bt_hci_transport_hooks* hooks, bluetooth_device** _device)
{

	bluetooth_device* device = new (std::nothrow) bluetooth_device;
	if (device == NULL)
		return B_NO_MEMORY;

	for (int index = 0; index < HCI_NUM_PACKET_TYPES; index++) {
		device->fBuffersRx[index] = NULL;
		device->fExpectedPacketSize[index] = 0;
	}

	device->info = NULL; // not yet used
	device->hooks = hooks;
	device->supportedPacketTypes = (HCI_DM1 | HCI_DH1 | HCI_HV1);
	device->linkMode = (HCI_LM_ACCEPT);
	device->mtu = L2CAP_MTU_MINIMUM; // TODO: ensure specs min value

	MutexLocker _(&sListLock);

	if (sDeviceList.IsEmpty())
		device->index = HCI_DEVICE_INDEX_OFFSET; // REVIEW: dev index
	else {
		device->index = (sDeviceList.Tail())->index + 1; // REVIEW!
		TRACE("%s: List not empty\n", __func__);
	}

	sDeviceList.Add(device);

	TRACE("%s: Device %" B_PRIx32 "\n", __func__, device->index);

	*_device = device;

	return B_OK;
}


status_t
UnregisterDriver(hci_id id)
{
	// HciConnection objects contain a pointer to the device.  Close every
	// dependent protocol endpoint while that pointer is still valid.
	btCoreData->RemoveConnections(id);

	MutexLocker locker(&sListLock);
	bluetooth_device* device = FindDeviceByID(id);

	if (device == NULL)
		return B_ERROR;

	if (device->GetDoublyLinkedListLink()->next != NULL
		|| device->GetDoublyLinkedListLink()->previous != NULL
		|| device == sDeviceList.Head())
		sDeviceList.Remove(device);

	delete device;

	return B_OK;
}


status_t
PostCommand(hci_id hciId, net_buffer* buffer)
{
	if (buffer == NULL)
		panic("passing null buffer");

	bluetooth_device* device = FindDeviceByID(hciId);
	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	buffer->protocol = BT_COMMAND;

	return device->hooks->SendCommand(hciId, buffer);
}


// PostACL
status_t
PostACL(hci_id hciId, net_buffer* buffer)
{
	net_buffer* curr_frame = NULL;
	net_buffer* next_frame = buffer;
	uint8 flag = HCI_ACL_PACKET_START;

	if (buffer == NULL)
		panic("passing null buffer");

	uint16 handle = buffer->type; // TODO: CodeHandler

	bluetooth_device* device = FindDeviceByID(hciId);

	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	// A Low Energy controller has no flush timeout to honour and rejects the
	// automatically flushable start flag outright, which would silently lose
	// every packet sent over such a link.
	HciConnection* connection = btCoreData->ConnectionByHandle(handle, hciId);
	if (connection != NULL && connection->low_energy)
		flag = HCI_ACL_PACKET_START_NO_FLUSH;

	// TODO: ATOMIC! any other thread should stop here
	do {
		// Divide packet if big enough
		curr_frame = next_frame;

		if (curr_frame->size > device->mtu) {
			next_frame = curr_frame;
			curr_frame = gBufferModule->split(next_frame, device->mtu);
			if (curr_frame == NULL)
				return B_NO_MEMORY;
		} else {
			next_frame = NULL;
		}

		// Attach acl header
		{
			NetBufferPrepend<struct hci_acl_header> bufferHeader(curr_frame);
			status_t status = bufferHeader.Status();
			if (status < B_OK) {
				// free the buffer
				continue;
			}

			bufferHeader->handle = pack_acl_handle_flags(handle, flag, 0);
			bufferHeader->alen = curr_frame->size - sizeof(struct hci_acl_header);
		}

		// Send to driver
		curr_frame->protocol = BT_ACL;

		// We could pass a cookie and avoid the driver fetch the Id
		device->hooks->SendACL(device->index, curr_frame);
		flag = HCI_ACL_PACKET_FRAGMENT;

	} while (next_frame != NULL);

	return B_OK;
}


status_t
PostSCO(hci_id hciId, net_buffer* buffer)
{
	bluetooth_device* device = FindDeviceByID(hciId);

	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	buffer->protocol = BT_SCO;

	return device->hooks->SendSCO(hciId, buffer);
}


status_t
PostESCO(hci_id hciId, net_buffer* buffer)
{
	return B_ERROR;
}


static int
dump_bluetooth_devices(int argc, char** argv)
{
	bluetooth_device*	device;

	DoublyLinkedList<bluetooth_device>::Iterator iterator
		= sDeviceList.GetIterator();

	while (iterator.HasNext()) {
		device = iterator.Next();
		kprintf("\tindex=%" B_PRIx32 " @%p hooks=%p\n", device->index,
			device, device->hooks);
	}

	return 0;
}


static status_t
bluetooth_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			status_t status;

			status = get_module(NET_BUFFER_MODULE_NAME,
				(module_info**)&gBufferModule);

			if (status < B_OK) {
				panic("no way Dude we need that!");
				return status;
			}

			status = get_module(BT_CORE_DATA_MODULE_NAME,
				(module_info**)&btCoreData);
			if (status < B_OK) {
				ERROR("%s: problem getting bt core data module\n", __func__);
				return status;
			}

			new (&sDeviceList) DoublyLinkedList<bluetooth_device>;
				// static C++ objects are not initialized in the module startup

			BluetoothRXPort = new BluetoothRawDataPort(BT_RX_PORT_NAME,
				(BluetoothRawDataPort::port_listener_func)&HciPacketHandler);

			if (BluetoothRXPort->Launch() != B_OK) {
				ERROR("%s: RX thread creation failed!\n", __func__);
				// we Cannot do much here ... avoid registering
			} else {
				TRACE("%s: RX thread launched!\n", __func__);
			}

			sLinkChangeSemaphore = create_sem(0, "bt sem");
			if (sLinkChangeSemaphore < B_OK) {
				put_module(NET_STACK_MODULE_NAME);
				ERROR("%s: Link change semaphore failed\n", __func__);
				return sLinkChangeSemaphore;
			}

			mutex_init(&sListLock, "bluetooth devices");

			// status = InitializeAclConnectionThread();
			ERROR("%s: Connection Thread error\n", __func__);

			add_debugger_command("btLocalDevices", &dump_bluetooth_devices,
				"Lists Bluetooth LocalDevices registered in the Stack");

			return B_OK;
		}

		case B_MODULE_UNINIT:
		{
			delete_sem(sLinkChangeSemaphore);

			mutex_destroy(&sListLock);
			put_module(NET_BUFFER_MODULE_NAME);
			put_module(NET_STACK_MODULE_NAME);
			put_module(BT_CORE_DATA_MODULE_NAME);
			remove_debugger_command("btLocalDevices", &dump_bluetooth_devices);
			// status_t status = QuitAclConnectionThread();

			return B_OK;
		}

		default:
			return B_ERROR;
	}
}


bt_hci_module_info sBluetoothModule = {
	{
		BT_HCI_MODULE_NAME, 
		B_KEEP_LOADED, 
		bluetooth_std_ops
	}, 
	RegisterDriver, 
	UnregisterDriver,
	FindDeviceByID, 
	PostTransportPacket, 
	PostCommand, 
	PostACL, 
	PostSCO, 
	PostESCO
};


module_info* modules[] = {
	(module_info*)&sBluetoothModule,
	NULL
};
