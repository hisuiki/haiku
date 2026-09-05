/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_SMP_CRYPTO_H_
#define _BLUETOOTH_SMP_CRYPTO_H_

#include "aes.h"
#include <stddef.h>

void smp_e(const uint8 key[16], uint8 data[16]);
void smp_c1(const uint8 key[16], const uint8 random[16], const uint8 preq[7],
	const uint8 pres[7], uint8 initiatorAddressType,
	const uint8 initiatorAddress[6], uint8 responderAddressType,
	const uint8 responderAddress[6], uint8 result[16]);
void smp_s1(const uint8 key[16], const uint8 random1[16],
	const uint8 random2[16], uint8 result[16]);

#endif
