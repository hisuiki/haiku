/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef _BTL2CAP_H_
#define _BTL2CAP_H_

#include <bluetooth/bluetooth.h>

// Read-only socket option at level BLUETOOTH_PROTO_L2CAP. The value is a
// host-endian uint16 containing the peer's negotiated receive MTU.
#define SO_L2CAP_OUTGOING_MTU 1

struct sockaddr_l2cap {
	uint8		l2cap_len;		/* total length */
	uint8		l2cap_family;	/* address family */
	uint16		l2cap_psm;		/* PSM (Protocol/Service Multiplexor) */
	bdaddr_t	l2cap_bdaddr;	/* address */
};


#endif // _BTL2CAP_H_
