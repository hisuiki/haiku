/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_HID_H_
#define _BLUETOOTH_HID_H_

#include <bluetooth/bluetooth.h>


#define BLUETOOTH_HID_PORT_NAME "bluetooth HID input"

/*! Asks the input device to open a classic HIDP connection. Sent by the
	Bluetooth server once service discovery has found the HID profile.
*/
#define BLUETOOTH_HID_CONNECT 'bhic'

struct bluetooth_hid_connect_request {
	bdaddr_t	address;
};

/*!	Carries one boot protocol report from a Low Energy peripheral. The HID
	over GATT support lives in the kernel, because the attribute protocol it
	rides on is a fixed L2CAP channel, so the reports arrive this way rather
	than over a socket the way classic ones do.
*/
#define BLUETOOTH_HID_REPORT 'bhir'

#define BLUETOOTH_HID_MAX_REPORT_SIZE 32

enum {
	BLUETOOTH_HID_KEYBOARD_REPORT	= 0,
	BLUETOOTH_HID_MOUSE_REPORT		= 1,
	// Report protocol mode: the layout is whatever the report map says, and
	// reportId names which of its reports this is.
	BLUETOOTH_HID_GENERIC_REPORT	= 2
};

struct bluetooth_hid_report {
	bdaddr_t	address;
	uint8		type;
	uint8		reportId;
	uint8		size;
	uint8		data[BLUETOOTH_HID_MAX_REPORT_SIZE];
};

/*!	Carries a peripheral's HID report descriptor. A device that offers no boot
	protocol mode describes its own report layout instead, and this is that
	description; reports from such a device arrive with a report id and have
	to be decoded against it.
*/
#define BLUETOOTH_HID_REPORT_MAP 'bhim'

#define BLUETOOTH_HID_MAX_REPORT_MAP 512

struct bluetooth_hid_report_map {
	bdaddr_t	address;
	uint16		size;
	uint8		data[BLUETOOTH_HID_MAX_REPORT_MAP];
};

/*! Sent when a Low Energy peripheral goes away, so that no key or button is
	left stuck down.
*/
#define BLUETOOTH_HID_DISCONNECTED 'bhid'

struct bluetooth_hid_disconnected {
	bdaddr_t	address;
};

#endif
