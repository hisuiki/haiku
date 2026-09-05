/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_SMP_H_
#define _BLUETOOTH_SMP_H_

#include <net_buffer.h>

struct HciConnection;

/* Security Manager protocol opcodes, Core spec Vol 3 Part H 3.3. */
#define SMP_PAIRING_REQUEST				0x01
#define SMP_PAIRING_RESPONSE			0x02
#define SMP_PAIRING_CONFIRM				0x03
#define SMP_PAIRING_RANDOM				0x04
#define SMP_PAIRING_FAILED				0x05
#define SMP_ENCRYPTION_INFORMATION		0x06
#define SMP_CENTRAL_IDENTIFICATION		0x07
#define SMP_IDENTITY_INFORMATION		0x08
#define SMP_IDENTITY_ADDRESS_INFORMATION 0x09
#define SMP_SIGNING_INFORMATION			0x0a
#define SMP_SECURITY_REQUEST			0x0b

/* IO capabilities. NoInputNoOutput is what selects Just Works pairing. */
#define SMP_IO_DISPLAY_ONLY				0x00
#define SMP_IO_DISPLAY_YES_NO			0x01
#define SMP_IO_KEYBOARD_ONLY			0x02
#define SMP_IO_NO_INPUT_NO_OUTPUT		0x03
#define SMP_IO_KEYBOARD_DISPLAY			0x04

/* Authentication requirements. */
#define SMP_AUTH_BONDING				0x01
#define SMP_AUTH_MITM					0x04
#define SMP_AUTH_SECURE_CONNECTIONS		0x08

/* Key distribution. */
#define SMP_DIST_ENC_KEY				0x01
#define SMP_DIST_ID_KEY					0x02
#define SMP_DIST_SIGN_KEY				0x04

#define SMP_MAX_ENCRYPTION_KEY_SIZE		16

status_t smp_receive(HciConnection* connection, net_buffer* buffer);
/*! Resumes a bond if there is one for this peer, so a device that has been
	paired before does not have to pair again. */
void smp_link_established(HciConnection* connection);
status_t smp_start_pairing(HciConnection* connection);
void smp_connection_closed(HciConnection* connection);

#endif // _BLUETOOTH_SMP_H_
