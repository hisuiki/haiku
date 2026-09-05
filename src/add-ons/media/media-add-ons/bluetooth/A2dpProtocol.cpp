/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "A2dpProtocol.h"

#include <string.h>

namespace A2dp {

bool
SelectConfiguration(const uint8_t* capabilities, size_t size, Codec codec,
	Configuration& configuration, uint32_t bitRate)
{
	bool transport = false;
	bool found = false;
	Configuration selected = {};
	selected.codec = codec;
	while (size != 0) {
		if (size < 2 || capabilities[1] > size - 2)
			return false;
		uint8_t category = capabilities[0];
		size_t length = capabilities[1];
		size_t entrySize = length + 2;
		const uint8_t* data = capabilities + 2;
		if (category == 1) {
			if (transport || length != 0)
				return false;
			transport = true;
		} else if (category == 7) {
			if (found || length < 2 || data[0] != 0 || data[1] != codec)
				return false;
			data += 2;
			length -= 2;
			if (codec == SBC) {
				// Joint stereo, 16 blocks, 8 subbands, loudness allocation.
				if (length != 4 || (data[0] & 0x11) != 0x11
					|| (data[1] & 0x15) != 0x15 || data[2] < 2
					|| data[2] > data[3] || data[2] > 53)
					return false;
				selected.data[0] = 0x11;
				selected.data[1] = 0x15;
				// At 48 kHz with joint stereo, 16 blocks and 8 subbands,
				// the SBC rate is approximately 39 + 6 * bitpool kb/s.
				uint32_t bitpool = bitRate <= 39000 ? 2
					: (bitRate - 39000 + 3000) / 6000;
				if (bitpool < data[2]) bitpool = data[2];
				if (bitpool > data[3]) bitpool = data[3];
				if (bitpool > 53) bitpool = 53;
				selected.data[2] = bitpool;
				selected.data[3] = bitpool;
				selected.size = 4;
				selected.bitRate = 39000 + bitpool * 6000;
			} else if (codec == AAC) {
				if (length != 6 || !(data[0] & 0xc0)
					|| (data[2] & 0x84) != 0x84)
					return false;
				uint32_t maximum = (uint32_t(data[3] & 0x7f) << 16)
					| (uint32_t(data[4]) << 8) | data[5];
				// Keep each 1024-sample AAC frame comfortably inside a
				// typical 672-byte A2DP media MTU.  Higher rates cause regular
				// RTP fragmentation and packet loss on slower controllers.
				bitRate = bitRate < kMinimumAacBitRate
					? kMinimumAacBitRate : bitRate;
				bitRate = bitRate > kMaximumAacBitRate
					? kMaximumAacBitRate : bitRate;
				selected.bitRate = maximum != 0 && maximum < bitRate
					? maximum : bitRate;
				selected.data[0] = data[0] & 0x40 ? 0x40 : 0x80;
				selected.data[1] = 0;
				selected.data[2] = 0x84;
				// The native AAC encoder uses variable frame sizes.
				if (!(data[3] & 0x80))
					return false;
				selected.data[3] = 0x80 | (selected.bitRate >> 16);
				selected.data[4] = selected.bitRate >> 8;
				selected.data[5] = selected.bitRate;
				selected.size = 6;
			} else if (codec == LDAC) {
				const uint8_t vendor[] = {0x2d, 0x01, 0, 0, 0xaa, 0};
				if (length != 8 || memcmp(data, vendor, 6) != 0
					|| !(data[6] & 0x10) || !(data[7] & 0x01))
					return false;
				memcpy(selected.data, vendor, 6);
				selected.data[6] = 0x10;
				selected.data[7] = 0x01;
				selected.size = 8;
				selected.bitRate = bitRate;
			} else
				return false;
			found = true;
		}
		capabilities += entrySize;
		size -= entrySize;
	}
	if (!transport || !found)
		return false;
	configuration = selected;
	return true;
}


size_t
MakeCapabilities(Codec codec, uint8_t* output)
{
	const uint8_t sbc[] = {0x11, 0x15, 2, 53};
	const uint8_t aac[] = {0xc0, 0, 0x84, 0x81, 0xf4, 0};
	const uint8_t ldac[] = {0x2d, 1, 0, 0, 0xaa, 0, 0x10, 1};
	const uint8_t* data = codec == SBC ? sbc : codec == AAC ? aac : ldac;
	size_t size = codec == SBC ? sizeof(sbc)
		: codec == AAC ? sizeof(aac) : sizeof(ldac);
	output[0] = 1;
	output[1] = 0;
	output[2] = 7;
	output[3] = size + 2;
	output[4] = 0;
	output[5] = codec;
	memcpy(output + 6, data, size);
	return size + 6;
}


void
MakeRtpHeader(uint8_t* output, uint16_t sequence, uint32_t timestamp,
	uint32_t ssrc, bool marker)
{
	output[0] = 0x80;
	output[1] = 96 | (marker ? 0x80 : 0);
	output[2] = sequence >> 8;
	output[3] = sequence;
	for (int i = 0; i < 4; i++) {
		output[4 + i] = timestamp >> (24 - 8 * i);
		output[8 + i] = ssrc >> (24 - 8 * i);
	}
}

} // namespace A2dp
