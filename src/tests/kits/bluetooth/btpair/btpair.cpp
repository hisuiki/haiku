/*
 * Connects to a Low Energy peripheral by address, the same way the Bluetooth
 * preferences window's Pair button does, so the stack can be exercised
 * without a mouse and a screen.
 */

#include <Application.h>
#include <Message.h>
#include <Messenger.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/LocalDevice.h>

#include <bluetoothserver_p.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static bool
parse_address(const char* text, bdaddr_t& address)
{
	unsigned int bytes[6];
	if (sscanf(text, "%x:%x:%x:%x:%x:%x", &bytes[5], &bytes[4], &bytes[3],
			&bytes[2], &bytes[1], &bytes[0]) != 6) {
		return false;
	}

	for (int i = 0; i < 6; i++)
		address.b[i] = (uint8)bytes[i];

	return true;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		printf("usage: btpair <address> [public|random]\n");
		return 1;
	}

	bdaddr_t address;
	if (!parse_address(argv[1], address)) {
		printf("not an address: %s\n", argv[1]);
		return 1;
	}

	uint8 addressType = 1;	// random, which is what a Low Energy mouse uses
	if (argc > 2 && strcmp(argv[2], "public") == 0)
		addressType = 0;

	BApplication application("application/x-vnd.haiku-btpair");

	Bluetooth::LocalDevice* local = Bluetooth::LocalDevice::GetLocalDevice();
	if (local == NULL) {
		printf("no local Bluetooth device\n");
		return 1;
	}

	printf("local device %s (id %" B_PRId32 ")\n",
		local->GetFriendlyName().String(), local->ID());

	BMessenger server(BLUETOOTH_SIGNATURE);
	if (!server.IsValid()) {
		printf("the Bluetooth server is not running\n");
		return 1;
	}

	BMessage request(BT_REQ_CREATE_CONN);
	request.AddInt32("hci_id", local->ID());
	request.AddData("bdaddr", B_ANY_TYPE, &address, sizeof(bdaddr_t));
	request.AddString("name", "");
	request.AddUInt32("record", 0);
	request.AddUInt16("packet type", 0);
	request.AddUInt8("pscan_rep_mode", 0);
	request.AddUInt8("pscan_mode", 0);
	request.AddUInt16("clock_offset", 0);
	request.AddUInt8("role_switch", 0);
	request.AddBool("low_energy", true);
	request.AddUInt8("bdaddr_type", addressType);

	if (server.SendMessage(&request) != B_OK) {
		printf("could not reach the Bluetooth server\n");
		return 1;
	}

	printf("connect requested for %s (%s address)\n", argv[1],
		addressType == 1 ? "random" : "public");
	return 0;
}
