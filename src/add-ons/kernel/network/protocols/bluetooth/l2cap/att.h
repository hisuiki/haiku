/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_ATT_H_
#define _BLUETOOTH_ATT_H_

#include <net_buffer.h>

struct HciConnection;

/* Attribute protocol opcodes, Core spec Vol 3 Part F 3.4. */
#define ATT_ERROR_RESPONSE				0x01
#define ATT_EXCHANGE_MTU_REQUEST		0x02
#define ATT_EXCHANGE_MTU_RESPONSE		0x03
#define ATT_FIND_INFORMATION_REQUEST	0x04
#define ATT_FIND_INFORMATION_RESPONSE	0x05
#define ATT_READ_BY_TYPE_REQUEST		0x08
#define ATT_READ_BY_TYPE_RESPONSE		0x09
#define ATT_READ_REQUEST				0x0a
#define ATT_READ_RESPONSE				0x0b
#define ATT_READ_BLOB_REQUEST			0x0c
#define ATT_READ_BLOB_RESPONSE			0x0d
#define ATT_READ_BY_GROUP_TYPE_REQUEST	0x10
#define ATT_READ_BY_GROUP_TYPE_RESPONSE	0x11
#define ATT_WRITE_REQUEST				0x12
#define ATT_WRITE_RESPONSE				0x13
#define ATT_HANDLE_VALUE_NOTIFICATION	0x1b
#define ATT_HANDLE_VALUE_INDICATION		0x1d
#define ATT_HANDLE_VALUE_CONFIRMATION	0x1e
#define ATT_WRITE_COMMAND				0x52

/* Errors that mean "come back once the link is encrypted". */
#define ATT_ERROR_INSUFFICIENT_AUTHENTICATION	0x05
#define ATT_ERROR_INSUFFICIENT_AUTHORIZATION	0x08
#define ATT_ERROR_INSUFFICIENT_ENCRYPTION		0x0f
#define ATT_ERROR_ATTRIBUTE_NOT_FOUND			0x0a

/* Assigned numbers used by HID over GATT. */
#define GATT_PRIMARY_SERVICE			0x2800
#define GATT_CHARACTERISTIC				0x2803
#define GATT_CLIENT_CHARACTERISTIC_CONFIGURATION 0x2902
#define GATT_HID_SERVICE				0x1812
#define GATT_PROTOCOL_MODE				0x2a4e
#define GATT_BOOT_KEYBOARD_INPUT_REPORT	0x2a22
#define GATT_BOOT_MOUSE_INPUT_REPORT	0x2a33
#define GATT_REPORT						0x2a4d
#define GATT_REPORT_MAP					0x2a4b
#define GATT_REPORT_REFERENCE			0x2908

/* Report reference types. */
#define GATT_REPORT_TYPE_INPUT			0x01

#define GATT_PROTOCOL_MODE_BOOT			0x00
#define GATT_PROTOCOL_MODE_REPORT		0x01

status_t att_receive(HciConnection* connection, net_buffer* buffer);
status_t att_start_discovery(HciConnection* connection);
void att_link_encrypted(HciConnection* connection);
void att_connection_closed(HciConnection* connection);

#endif // _BLUETOOTH_ATT_H_
