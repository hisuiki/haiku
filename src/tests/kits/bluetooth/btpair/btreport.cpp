/*
 * Posts a synthetic HID report to the Bluetooth input device, so the path
 * from the kernel's HID over GATT support through to the input server can be
 * exercised without moving a real mouse.
 */

#include <BluetoothHID.h>

#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int
main(int argc, char** argv)
{
	if (argc < 4) {
		printf("usage: btreport mouse <dx> <dy> [buttons]\n");
		printf("       btreport key <usage> [modifiers]\n");
		return 1;
	}

	port_id port = find_port(BLUETOOTH_HID_PORT_NAME);
	if (port < B_OK) {
		printf("the bluetooth_hid input device is not running\n");
		return 1;
	}

	bluetooth_hid_report report;
	memset(&report, 0, sizeof(report));

	// Any address will do: the add-on keeps state per peripheral and makes
	// one the first time it hears from it.
	report.address.b[0] = 0x82;
	report.address.b[1] = 0x7f;
	report.address.b[2] = 0xdb;
	report.address.b[3] = 0xce;
	report.address.b[4] = 0x97;
	report.address.b[5] = 0xe5;

	if (strcmp(argv[1], "mouse") == 0) {
		report.type = BLUETOOTH_HID_MOUSE_REPORT;
		report.size = 3;
		report.data[0] = argc > 4 ? (uint8)atoi(argv[4]) : 0;
		report.data[1] = (uint8)(int8)atoi(argv[2]);
		report.data[2] = (uint8)(int8)atoi(argv[3]);
	} else {
		report.type = BLUETOOTH_HID_KEYBOARD_REPORT;
		report.size = 8;
		report.data[0] = argc > 3 ? (uint8)atoi(argv[3]) : 0;
		report.data[2] = (uint8)atoi(argv[2]);
	}

	status_t status = write_port(port, BLUETOOTH_HID_REPORT, &report,
		sizeof(report));
	if (status != B_OK) {
		printf("could not post the report: %s\n", strerror(status));
		return 1;
	}

	printf("report posted\n");
	return 0;
}
