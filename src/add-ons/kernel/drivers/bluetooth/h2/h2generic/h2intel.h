/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _H2INTEL_H_
#define _H2INTEL_H_


#include "h2generic.h"


#define BT_INTEL_SECURE_BOOT	(1 << 3)

status_t intel_bluetooth_setup(bt_usb_dev* device);


#endif
