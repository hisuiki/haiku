/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "A2dpProtocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main()
{
	using namespace A2dp;
	// Independent wire fixtures: stereo sink capabilities, including options
	// this implementation deliberately does not select.
	uint8_t sbc[] = {1, 0, 7, 6, 0, 0, 0xff, 0xff, 2, 53};
	uint8_t aac[] = {1, 0, 7, 8, 0, 2, 0xc0, 0xff, 0xfc, 0x83, 0xe8, 0};
	uint8_t ldac[] = {1, 0, 7, 10, 0, 255, 0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 7};
	Configuration config;
	assert(SelectConfiguration(sbc, sizeof(sbc), SBC, config));
	assert(config.data[0] == 0x11 && config.data[1] == 0x15);
	assert(config.data[2] == 15 && config.data[3] == 15
		&& config.bitRate == 129000);
	assert(SelectConfiguration(aac, sizeof(aac), AAC, config));
	assert(config.bitRate == kDefaultBitRate && config.data[2] == 0x84);
	assert(SelectConfiguration(aac, sizeof(aac), AAC, config, 64000));
	assert(config.bitRate == 64000);
	assert(SelectConfiguration(ldac, sizeof(ldac), LDAC, config));
	assert(config.data[6] == 0x10 && config.data[7] == 1
		&& config.bitRate == 128000);
	assert(SelectConfiguration(ldac, sizeof(ldac), LDAC, config, 256000));
	assert(config.bitRate == 256000);
	assert(!SelectConfiguration(ldac, sizeof(ldac), AAC, config));
	for (size_t i = 0; i < sizeof(ldac); i++)
		assert(!SelectConfiguration(ldac, i, LDAC, config));
	ldac[6] = 0x4f; // Another vendor's codec is not LDAC.
	assert(!SelectConfiguration(ldac, sizeof(ldac), LDAC, config));
	sbc[8] = 54;
	assert(!SelectConfiguration(sbc, sizeof(sbc), SBC, config));
	sbc[8] = 2;
	sbc[9] = 35;
	assert(SelectConfiguration(sbc, sizeof(sbc), SBC, config, 256000));
	assert(config.data[2] == 35 && config.data[3] == 35);
	aac[9] = 0x81;
	aac[10] = 0xf4;
	aac[11] = 0;
	assert(SelectConfiguration(aac, sizeof(aac), AAC, config));
	assert(config.bitRate == 128000);
	aac[9] = 1; // No VBR support.
	assert(!SelectConfiguration(aac, sizeof(aac), AAC, config));
	uint8_t malformed[] = {1, 0, 7, 255};
	assert(!SelectConfiguration(malformed, sizeof(malformed), SBC, config));
	uint8_t rtp[12];
	MakeRtpHeader(rtp, 0x1234, 0x56789abc, 0xdef01234, true);
	const uint8_t expected[] = {0x80, 0xe0, 0x12, 0x34, 0x56, 0x78,
		0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34};
	assert(memcmp(rtp, expected, sizeof(rtp)) == 0);
	// Exercise length parsing with deterministic random inputs under ASan.
	uint32_t random = 42;
	for (int trial = 0; trial < 50000; trial++) {
		uint8_t data[64];
		for (size_t i = 0; i < sizeof(data); i++) {
			random = random * 1664525 + 1013904223;
			data[i] = random >> 24;
		}
		SelectConfiguration(data, trial % sizeof(data), LDAC, config);
	}
	puts("Bluetooth audio protocol tests passed");
	return 0;
}
