/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <bluetooth/DeviceClass.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <bluetooth/debug.h>
#include <bluetooth/bluetooth_error.h>

#include <Catalog.h>
#include <CommandManager.h>
#include <Locale.h>

#include "KitSupport.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RemoteDevice"


namespace Bluetooth {

bool
RemoteDevice::IsTrustedDevice(void)
{
	CALLED();
	return true;
}


BObjectList<RemoteDevice>
RemoteDevice::GetRemoteDevices(LocalDevice* localDevice)
{
	BObjectList<RemoteDevice> rdList;

	if (localDevice == NULL)
		return rdList;

	BMessage devices;
	BMessage requestRemoteDevices(BT_MSG_GET_REMOTE_DEVICES);
	requestRemoteDevices.AddInt32("hci_id", localDevice->ID());
	if (BMessenger(BLUETOOTH_SIGNATURE).SendMessage(&requestRemoteDevices,
		&devices) != B_OK)
		return rdList;

	BMessage device;
	for (int32 i = 0; devices.FindMessage("remote", i, &device) == B_OK; i++) {
		bdaddr_t* bdaddr;
		uint8* class_of_device;
		ssize_t size;

		device.FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &size);
		device.FindData("class_of_device", B_ANY_TYPE, (const void**)&class_of_device, &size);
		RemoteDevice* rd = new RemoteDevice(*bdaddr, class_of_device);

		rd->fDiscovererLocalDevice = localDevice;
		device.FindString("name", &rd->fFriendlyName);
		device.FindUInt16("clock_offset", &rd->fClockOffset);
		device.FindUInt8("pscan_rep_mode", &rd->fPageRepetitionMode);

		rdList.AddItem(rd);
	}

	return rdList;
}


RemoteDevice*
RemoteDevice::ParseRemoteDevice(LocalDevice* localDevice, BMessage* message)
{
	bdaddr_t* bdaddr;
	uint8* class_of_device;
	ssize_t size;

	message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &size);
	message->FindData("class_of_device", B_ANY_TYPE, (const void**)&class_of_device, &size);
	RemoteDevice* rd = new RemoteDevice(*bdaddr, class_of_device);

	// TODO: we should request the remote device's name and other parameters.
	rd->fDiscovererLocalDevice = localDevice;

	return rd;
}


// Paging a device that is not listening takes the page timeout, 5.12 seconds
// by default, and this happens on the thread drawing the window.
static const bigtime_t kNameRequestTimeout = 10000000;


/*!	Falls back to the address, which at least identifies the device, rather
	than to a placeholder that says nothing.
*/
BString
RemoteDevice::_BestKnownName()
{
	if (!fFriendlyName.IsEmpty())
		return fFriendlyName;

	return bdaddrUtils::ToString(fBdaddr);
}


BString
RemoteDevice::GetFriendlyName(bool alwaysAsk)
{
	CALLED();

	if (!fFriendlyName.IsEmpty() && fFriendlyNameIsComplete)
		return fFriendlyName;

	// Only an explicit request pages the device. Discovery already fills the
	// name in from the inquiry response or the advertising data, and when it
	// did not, showing the address beats stalling the caller - which is the
	// thread drawing the window - on a page that may never be answered.
	if (!alwaysAsk)
		return _BestKnownName();

	// A Low Energy peripheral cannot be paged at all, so there is nothing to
	// ask even when the caller insists.
	if (fLowEnergy)
		return _BestKnownName();

	// Asking again costs another page timeout and would almost certainly fail
	// the same way.
	if (fNameRequestFailed)
		return _BestKnownName();

	if (fDiscovererLocalDevice == NULL)
		return BString(B_TRANSLATE("#NoOwnerError#Not Valid name"));

	if (fMessenger == NULL)
		return BString(B_TRANSLATE("#ServerNotReady#Not Valid name"));

	void* remoteNameCommand = NULL;
	size_t size;

	// Issue inquiry command
	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", fDiscovererLocalDevice->ID());

	// Fill the request
	remoteNameCommand = buildRemoteNameRequest(fBdaddr, fPageRepetitionMode,
		fClockOffset, &size);

	request.AddData("raw command", B_ANY_TYPE, remoteNameCommand, size);

	request.AddInt16("eventExpected",  HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_REMOTE_NAME_REQUEST));

	request.AddInt16("eventExpected",  HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE);


	// A device that has gone away answers nothing at all, and this runs on the
	// thread drawing the window.
	if (fMessenger->SendMessage(&request, &reply, kNameRequestTimeout,
			kNameRequestTimeout) == B_OK) {
		BString name;
		uint8 status;

		if ((reply.FindUInt8("status", &status) == B_OK) && (status == BT_OK)) {

			if ((reply.FindString("friendlyname", &name) == B_OK )) {
				fFriendlyName = name;
				fFriendlyNameIsComplete = true;
				return name;
			} else {
				fNameRequestFailed = true;
				return _BestKnownName();
			}

		} else {
			// seems we got a negative event
			fNameRequestFailed = true;
			return _BestKnownName();
		}
	}

	fNameRequestFailed = true;
	return _BestKnownName();
}


BString
RemoteDevice::GetFriendlyName()
{
	CALLED();
	return GetFriendlyName(false);
}


BString
RemoteDevice::GetCachedFriendlyName()
{
	CALLED();
	return fFriendlyName;
}


bdaddr_t
RemoteDevice::GetBluetoothAddress()
{
	CALLED();
	return fBdaddr;
}


uint16
RemoteDevice::GetClockOffset()
{
	return fClockOffset;
}


uint8
RemoteDevice::GetPageRepetitionMode()
{
	return fPageRepetitionMode;
}


bool
RemoteDevice::Equals(RemoteDevice* obj)
{
	CALLED();
	return bdaddrUtils::Compare(fBdaddr, obj->GetBluetoothAddress());
}


status_t
RemoteDevice::Connect()
{
	CALLED();
	BMessage request(BT_REQ_CREATE_CONN);

	request.AddInt32("hci_id", fDiscovererLocalDevice->ID());

	bdaddr_t bdaddr = GetBluetoothAddress();
	request.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));

	request.AddString("name", GetFriendlyName());
	request.AddUInt32("record", fDeviceClass.Record());

	uint16 packetType;
	fDiscovererLocalDevice->GetProperty("packet_type", (uint32*)&packetType);
	request.AddUInt16("packet type", packetType);

	request.AddUInt8("pscan_rep_mode", fPageRepetitionMode);
	request.AddUInt8("pscan_mode", fScanMode);
	request.AddUInt16("clock_offset", fClockOffset);

	uint8 roleSwitch;
	fDiscovererLocalDevice->GetProperty("role_switch_capable", (uint32*)&roleSwitch);
	request.AddUInt8("role_switch", roleSwitch);

	// A Low Energy peripheral needs an entirely different connect command, and
	// its address type is part of how it is reached.
	request.AddBool("low_energy", fLowEnergy);
	request.AddUInt8("bdaddr_type", fAddressType);

	if (fMessenger->SendMessage(&request) == B_OK)
		return B_OK;
	return B_ERROR;
}


status_t
RemoteDevice::CancelConnection()
{
	CALLED();
	BMessage request(BT_REQ_CANCEL_CONN);

	request.AddInt32("hci_id", fDiscovererLocalDevice->ID());

	bdaddr_t bdaddr = GetBluetoothAddress();
	request.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));

	if (fMessenger->SendMessage(&request) == B_OK)
		return B_OK;
	return B_ERROR;
}


status_t
RemoteDevice::Disconnect(bool removeDevice)
{
	CALLED();
	BMessage request(BT_REQ_DISCONNECT);
	if (removeDevice)
		request.what = BT_REQ_REMOVE_DEVICE;

	request.AddInt32("hci_id", fDiscovererLocalDevice->ID());

	bdaddr_t bdaddr = GetBluetoothAddress();
	request.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
	request.AddUInt8("reason", BT_REMOTE_USER_ENDED_CONNECTION);

	if (fMessenger->SendMessage(&request) == B_OK)
		return B_OK;
	return B_ERROR;
}


//  static RemoteDevice* GetRemoteDevice(Connection conn);
//  bool Authorize(Connection conn);
//  bool Encrypt(Connection conn, bool on);


RemoteDevice::ConnectionState
RemoteDevice::GetConnectionState()
{
	CALLED();
	BMessage request(BT_REQ_CONN_STATE);
	BMessage reply;

	bdaddr_t bdaddr = GetBluetoothAddress();
	request.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));

	if (fMessenger->SendMessage(&request, &reply) != B_OK)
		return RemoteDevice::DISCONNECTED;

	uint8 conn_state;
	reply.FindUInt8("conn state", &conn_state);
	return static_cast<RemoteDevice::ConnectionState>(conn_state);
}


//  bool IsAuthorized(Connection conn);


bool
RemoteDevice::IsEncrypted()
{
	CALLED();
	return true;
}


LocalDevice*
RemoteDevice::GetLocalDeviceOwner()
{
	CALLED();
	return fDiscovererLocalDevice;
}


/* Private */
void
RemoteDevice::SetLocalDeviceOwner(LocalDevice* ld)
{
	CALLED();
	fDiscovererLocalDevice = ld;
}


/* Constructor */
RemoteDevice::RemoteDevice(const bdaddr_t address, uint8 record[3])
	:
	BluetoothDevice(),
	fDiscovererLocalDevice(NULL),
	fScanMode(0),
	fFriendlyNameIsComplete(false),
	fLowEnergy(false),
	fAddressType(LE_PUBLIC_ADDRESS),
	fNameRequestFailed(false)
{
	CALLED();
	fBdaddr = address;
	fDeviceClass.SetRecord(record);
	fMessenger = _RetrieveBluetoothMessenger();
}


RemoteDevice::RemoteDevice(const BString& address)
	:
	BluetoothDevice(),
	fDiscovererLocalDevice(NULL),
	fScanMode(0),
	fFriendlyNameIsComplete(false),
	fLowEnergy(false),
	fAddressType(LE_PUBLIC_ADDRESS),
	fNameRequestFailed(false)
{
	CALLED();
	fBdaddr = bdaddrUtils::FromString((const char*)address.String());
	fMessenger = _RetrieveBluetoothMessenger();
}


RemoteDevice::~RemoteDevice()
{
	CALLED();
	delete fMessenger;
}


BString
RemoteDevice::GetProperty(const char* property) /* Throwing */
{
	return NULL;
}


status_t
RemoteDevice::GetProperty(const char* property, uint32* value) /* Throwing */
{
	CALLED();
	return B_ERROR;
}


DeviceClass
RemoteDevice::GetDeviceClass()
{
	CALLED();
	return fDeviceClass;
}

} /* end namespace Bluetooth */
