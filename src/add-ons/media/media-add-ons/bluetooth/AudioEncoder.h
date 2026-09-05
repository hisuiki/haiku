/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_AUDIO_ENCODER_H
#define BLUETOOTH_AUDIO_ENCODER_H

#include "A2dpProtocol.h"
#include <SupportDefs.h>
#include "ldacBT.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

class AudioEncoder {
public:
	typedef status_t (*PacketWriter)(void*, const uint8*, size_t, unsigned);
	AudioEncoder();
	~AudioEncoder();
	status_t Init(const A2dp::Configuration& config, uint16 mtu,
		PacketWriter writer, void* cookie);
	void Reset();
	status_t Write(const float* stereo, size_t frames);

private:
	status_t _Encode();
	A2dp::Codec fCodec;
	HANDLE_LDAC_BT fLdac;
	AVCodecContext* fContext;
	AVFormatContext* fMux;
	AVFrame* fFrame;
	AVPacket* fPacket;
	float fPCM[2048];
	size_t fUsed;
	size_t fFrameSize;
	int64 fSamples;
	PacketWriter fWriter;
	void* fCookie;
};
#endif
