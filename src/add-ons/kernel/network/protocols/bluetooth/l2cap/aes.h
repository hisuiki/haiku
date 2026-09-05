/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_AES_H_
#define _BLUETOOTH_AES_H_

#include <SupportDefs.h>

struct aes128_context {
	uint8	round_keys[176];
};

void aes128_set_key(aes128_context* context, const uint8 key[16]);
void aes128_encrypt(const aes128_context* context, const uint8 in[16],
	uint8 out[16]);

#endif
