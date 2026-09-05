/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <bluetooth/bluetooth.h>

#define BLUETOOTH_AUDIO_PORT "bluetooth audio output"
#define BLUETOOTH_AUDIO_CONNECT 'bacn'
#define BLUETOOTH_AUDIO_DISCONNECT 'badc'

struct bluetooth_audio_connect {
	bdaddr_t address;
};

#endif
