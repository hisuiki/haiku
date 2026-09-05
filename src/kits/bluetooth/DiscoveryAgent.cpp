/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <bluetooth/bluetooth_error.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/debug.h>

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>

#include "KitSupport.h"


namespace Bluetooth {


/*!	Bit N of the HCI event mask enables the event with code N + 1. The
	controller's default mask stops at bit 44, which leaves out the Simple
	Pairing events and, more importantly here, the LE Meta event that carries
	advertising reports. Without it a Low Energy scan runs but reports nothing.
*/
static const uint64 kEventMask = 0x00001FFFFFFFFFFFULL	// controller default
	| (1ULL << 46)	// Extended Inquiry Result
	| (1ULL << 47)	// Encryption Key Refresh Complete
	| (1ULL << 48)	// IO Capability Request
	| (1ULL << 49)	// IO Capability Response
	| (1ULL << 50)	// User Confirmation Request
	| (1ULL << 51)	// User Passkey Request
	| (1ULL << 52)	// Remote OOB Data Request
	| (1ULL << 53)	// Simple Pairing Complete
	| (1ULL << 61);	// LE Meta

// Scan for 30 ms out of every 60 ms. Frequent enough to catch a peripheral
// that only advertises while a button is held, without hogging the radio.
static const uint16 kLeScanInterval = 0x0060;
static const uint16 kLeScanWindow = 0x0030;


// A controller that never answers a command it does not implement would
// otherwise block the calling thread, which is the one running the user
// interface, for good.
static const bigtime_t kCommandReplyTimeout = 5000000;


/*!	Sends one command and waits for its Command Complete. Takes over the
	command buffer that the build* functions allocate.
*/
static status_t
send_simple_command(BMessenger* messenger, hci_id hciId, void* command,
	size_t size, uint16 opcode)
{
	if (command == NULL)
		return B_NO_MEMORY;

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hciId);
	request.AddData("raw command", B_ANY_TYPE, command, size);
	request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	request.AddInt16("opcodeExpected", opcode);

	status_t status = messenger->SendMessage(&request, &reply,
		kCommandReplyTimeout, kCommandReplyTimeout);
	free(command);

	return status;
}


/*!	Low Energy peripherals never answer an inquiry, they advertise instead, so
	discovering them means running a scan alongside the inquiry.
*/
static status_t
set_le_scan_enabled(BMessenger* messenger, hci_id hciId, bool enable)
{
	size_t size = 0;
	status_t status = B_OK;

	if (enable) {
		// Active scanning, so that peripherals are asked for the scan response
		// that usually carries their name.
		void* command = buildLeSetScanParameters(LE_SCAN_TYPE_ACTIVE,
			kLeScanInterval, kLeScanWindow, LE_OWN_ADDRESS_PUBLIC,
			LE_SCAN_FILTER_ACCEPT_ALL, &size);

		status = send_simple_command(messenger, hciId, command, size,
			PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_PARAMETERS));

		if (status != B_OK)
			return status;
	}

	// Filter duplicates: a peripheral advertises several times a second, and
	// the discovery listener already merges repeated reports by address. The
	// scan response, which is what usually carries the name, is a separate
	// report and still gets through.
	void* command = buildLeSetScanEnable(enable, true, &size);

	return send_simple_command(messenger, hciId, command, size,
		PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_ENABLE));
}


RemoteDevicesList
DiscoveryAgent::RetrieveDevices(int option)
{
	CALLED();
    // No inquiry process initiated
    if (fLastUsedListener == NULL)
        return RemoteDevicesList();

    return fLastUsedListener->GetRemoteDevicesList();
}


status_t
DiscoveryAgent::StartInquiry(int accessCode, DiscoveryListener* listener)
{
	CALLED();
    return StartInquiry(accessCode, listener, GetInquiryTime());
}


status_t
DiscoveryAgent::StartInquiry(uint32 accessCode, DiscoveryListener* listener,
	bigtime_t secs)
{
	CALLED();
    size_t size;

	if (fMessenger == NULL)
		return B_ERROR;

	if (secs < 1 || secs > 61 )
		return B_TIMED_OUT;

    void*  startInquiryCommand = NULL;

    // keep the listener whats the current listener for our inquiry state
    fLastUsedListener = listener;

    // Inform the listener who is gonna be its owner LocalDevice
    // and its discovered devices
    listener->SetLocalDeviceOwner(fLocalDevice);

    /* Issue inquiry command */
    BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
    BMessage reply;

    request.AddInt32("hci_id", fLocalDevice->ID());

    startInquiryCommand = buildInquiry(accessCode, secs, BT_MAX_RESPONSES,
		&size);

    // For stating the inquiry
    request.AddData("raw command", B_ANY_TYPE, startInquiryCommand, size);
    request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
    request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY));

	// For getting each discovered message
	request.AddInt16("eventExpected",  HCI_EVENT_INQUIRY_RESULT);
	request.AddInt16("eventExpected", HCI_EVENT_INQUIRY_RESULT_WITH_RSSI);
	request.AddInt16("eventExpected", HCI_EVENT_EXTENDED_INQUIRY_RESULT);

	// For finishing each discovered message
	request.AddInt16("eventExpected",  HCI_EVENT_INQUIRY_COMPLETE);

	// And for the Low Energy peripherals, which report themselves through the
	// LE Meta event rather than answering the inquiry above.
	request.AddInt16("eventExpected", HCI_EVENT_LE_META);

	// The LE Meta event is masked off by default, so it has to be unmasked
	// before the scan is of any use.
	size_t maskSize = 0;
	void* eventMaskCommand = buildSetEventMask(kEventMask, &maskSize);

	send_simple_command(fMessenger, fLocalDevice->ID(), eventMaskCommand,
		maskSize, PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_SET_EVENT_MASK));

	// A standard inquiry result carries no name, and a device cannot be paged
	// for one while the inquiry is still running. Extended results are what
	// make the name arrive together with the device itself.
	size_t modeSize = 0;
	void* inquiryModeCommand = buildWriteInquiryMode(
		HCI_INQUIRY_MODE_RSSI_OR_EIR, &modeSize);

	send_simple_command(fMessenger, fLocalDevice->ID(), inquiryModeCommand,
		modeSize, PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_INQUIRY_MODE));

	// A controller without Low Energy support just fails these, which is not a
	// reason to skip the inquiry.
	set_le_scan_enabled(fMessenger, fLocalDevice->ID(), true);

	if (fMessenger->SendMessage(&request, listener) == B_OK)
		return B_OK;

	return B_ERROR;
}


status_t
DiscoveryAgent::CancelInquiry(DiscoveryListener* listener)
{
	CALLED();
    size_t size;

    if (fMessenger == NULL)
    	return B_ERROR;

    void* cancelInquiryCommand = NULL;
    uint8 bt_status = BT_ERROR;

    /* Issue inquiry command */
    BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
    BMessage reply;

    request.AddInt32("hci_id", fLocalDevice->ID());

    // The scan is not part of the inquiry, so it has to be stopped separately.
    set_le_scan_enabled(fMessenger, fLocalDevice->ID(), false);

    cancelInquiryCommand = buildInquiryCancel(&size);
    request.AddData("raw command", B_ANY_TYPE, cancelInquiryCommand, size);
    request.AddInt16("eventExpected",  HCI_EVENT_CMD_COMPLETE);
    request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY_CANCEL));

    if (fMessenger->SendMessage(&request, &reply) == B_OK) {
        if (reply.FindUInt8("status", &bt_status ) == B_OK ) {
			return bt_status;
		}
    }

    return B_ERROR;
}


void
DiscoveryAgent::SetLocalDeviceOwner(LocalDevice* ld)
{
	CALLED();
    fLocalDevice = ld;
}


DiscoveryAgent::DiscoveryAgent(LocalDevice* ld)
{
	CALLED();
	fLocalDevice = ld;
	fMessenger = _RetrieveBluetoothMessenger();
}


DiscoveryAgent::~DiscoveryAgent()
{
	CALLED();
	delete fMessenger;
}


} /* End namespace Bluetooth */
