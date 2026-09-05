/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef A2DP_PROTOCOL_H
#define A2DP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace A2dp {

static const uint32_t kDefaultBitRate = 128000;
static const uint32_t kMinimumAacBitRate = 64000;
static const uint32_t kMaximumAacBitRate = 128000;

enum Codec { SBC = 0, AAC = 2, LDAC = 255 };
enum Signal {
	DISCOVER = 1, GET_CAPABILITIES, SET_CONFIGURATION, GET_CONFIGURATION,
	RECONFIGURE, OPEN, START, CLOSE, SUSPEND, ABORT, SECURITY_CONTROL,
	GET_ALL_CAPABILITIES, DELAY_REPORT
};

struct Configuration {
	Codec codec;
	uint8_t seid;
	uint8_t data[8];
	size_t size;
	uint32_t bitRate;
};

// Select 48 kHz stereo from a complete AVDTP service capability list.
// No compiler bitfields: every value below is in wire order.
bool SelectConfiguration(const uint8_t* capabilities, size_t size, Codec codec,
	Configuration& configuration,
	uint32_t bitRate = kDefaultBitRate);
size_t MakeCapabilities(Codec codec, uint8_t* output);
void MakeRtpHeader(uint8_t* output, uint16_t sequence, uint32_t timestamp,
	uint32_t ssrc, bool marker);

} // namespace A2dp
#endif
