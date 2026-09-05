/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "ldacBT.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


static void
testBitRate(int requested, int expected, size_t expectedFrameSize)
{
	HANDLE_LDAC_BT encoder = ldacBT_get_handle();
	assert(encoder != NULL);
	assert(ldacBT_init_handle_encode(encoder, 679, LDACBT_EQMID_MQ,
		LDACBT_CHANNEL_MODE_STEREO, LDACBT_SMPL_FMT_F32, 48000) == 0);
	assert(ldacBT_set_bitrate(encoder, requested) == 0);
	assert(ldacBT_get_bitrate(encoder) == expected);

	float pcm[LDACBT_ENC_LSU * 2];
	memset(pcm, 0, sizeof(pcm));
	uint8_t stream[LDACBT_MAX_NBYTES];
	int streamSize = 0;
	int frameCount = 0;
	for (int attempt = 0; attempt < 32 && streamSize == 0; attempt++) {
		int used = 0;
		assert(ldacBT_encode(encoder, pcm, &used, stream, &streamSize,
			&frameCount) == 0);
		assert(used == (int)sizeof(pcm));
	}
	assert(streamSize > 0 && frameCount > 0);
	size_t offset = 0;
	for (int frame = 0; frame < frameCount; frame++) {
		assert(offset + 3 <= (size_t)streamSize && stream[offset] == 0xaa);
		size_t frameSize = (((stream[offset + 1] & 7) << 6)
			| (stream[offset + 2] >> 2)) + 4;
		assert(frameSize == expectedFrameSize);
		offset += frameSize;
	}
	assert(offset == (size_t)streamSize);
	ldacBT_free_handle(encoder);
}


int
main()
{
	// At 48 kHz an LDAC transport frame represents 3 kb/s per byte.
	// These are the closest frame lengths to the requested UI values.
	testBitRate(128, 129, 43);
	testBitRate(256, 255, 85);
	puts("LDAC 128/256 kb/s bitrate and framing tests passed");
	return 0;
}
