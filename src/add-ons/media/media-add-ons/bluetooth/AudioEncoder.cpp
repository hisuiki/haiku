/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "AudioEncoder.h"

#include <algorithm>
#include <cmath>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

AudioEncoder::AudioEncoder()
	:
	fCodec(A2dp::SBC), fLdac(NULL), fContext(NULL), fMux(NULL),
	fFrame(NULL), fPacket(NULL), fUsed(0), fFrameSize(0), fSamples(0),
	fWriter(NULL), fCookie(NULL)
{
}

AudioEncoder::~AudioEncoder()
{
	Reset();
}

void
AudioEncoder::Reset()
{
	if (fLdac != NULL)
		ldacBT_free_handle(fLdac);
	fLdac = NULL;
	av_frame_free(&fFrame);
	av_packet_free(&fPacket);
	avcodec_free_context(&fContext);
	avformat_free_context(fMux);
	fMux = NULL;
	fUsed = fFrameSize = 0;
	fSamples = 0;
}

status_t
AudioEncoder::Init(const A2dp::Configuration& config, uint16 mtu,
	PacketWriter writer, void* cookie)
{
	Reset();
	fCodec = config.codec;
	fWriter = writer;
	fCookie = cookie;
	if (fCodec == A2dp::LDAC) {
		fLdac = ldacBT_get_handle();
		if (fLdac == NULL)
			return B_NO_MEMORY;
		// libldac batches for a 2-DH5 radio packet (minimum 679 bytes).
		// The session splits that batch into individual LDAC frames before
		// applying the L2CAP MTU, which can be smaller than a radio packet.
		if (mtu < 343)
			return B_NOT_SUPPORTED;
		if (ldacBT_init_handle_encode(fLdac, std::max<int>(mtu, 679),
				LDACBT_EQMID_MQ,
				LDACBT_CHANNEL_MODE_STEREO, LDACBT_SMPL_FMT_F32, 48000) != 0)
			return B_NOT_SUPPORTED;
		if (ldacBT_set_bitrate(fLdac, config.bitRate / 1000) != 0)
			return B_NOT_SUPPORTED;
		fFrameSize = LDACBT_ENC_LSU;
		return B_OK;
	}
	const AVCodec* codec = avcodec_find_encoder(
		fCodec == A2dp::AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_SBC);
	if (codec == NULL)
		return B_NOT_SUPPORTED;
	fContext = avcodec_alloc_context3(codec);
	fFrame = av_frame_alloc();
	fPacket = av_packet_alloc();
	if (fContext == NULL || fFrame == NULL || fPacket == NULL)
		return B_NO_MEMORY;
	fContext->sample_rate = 48000;
	av_channel_layout_default(&fContext->ch_layout, 2);
	fContext->time_base = AVRational{1, 48000};
	fContext->sample_fmt = fCodec == A2dp::AAC
		? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
	if (fCodec == A2dp::AAC) {
		fContext->bit_rate = config.bitRate;
		fContext->profile = FF_PROFILE_AAC_LOW;
		fContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	} else {
		// FFmpeg selects joint stereo at this rate. Explicit quality fixes
		// the bitpool; max_delay selects 16 blocks and 8 subbands.
		fContext->bit_rate = 128000;
		fContext->global_quality = config.data[3] * FF_QP2LAMBDA;
		av_opt_set_int(fContext->priv_data, "sbc_delay", 13000, 0);
	}
	if (avcodec_open2(fContext, codec, NULL) < 0)
		return B_NOT_SUPPORTED;
	fFrameSize = fContext->frame_size;
	if (fFrameSize == 0 || fFrameSize > 1024)
		return B_BAD_DATA;
	fFrame->nb_samples = fFrameSize;
	fFrame->format = fContext->sample_fmt;
	fFrame->sample_rate = 48000;
	if (av_channel_layout_copy(&fFrame->ch_layout, &fContext->ch_layout) < 0
		|| av_frame_get_buffer(fFrame, 0) < 0)
		return B_NO_MEMORY;
	if (fCodec == A2dp::AAC) {
		if (avformat_alloc_output_context2(&fMux, NULL, "latm", NULL) < 0)
			return B_NOT_SUPPORTED;
		AVStream* stream = avformat_new_stream(fMux, NULL);
		if (stream == NULL || avcodec_parameters_from_context(stream->codecpar,
				fContext) < 0)
			return B_NO_MEMORY;
		stream->time_base = fContext->time_base;
		// Repeat StreamMuxConfig so a lost packet never loses configuration.
		av_opt_set_int(fMux->priv_data, "smc-interval", 1, 0);
		if (avio_open_dyn_buf(&fMux->pb) < 0)
			return B_NO_MEMORY;
		int result = avformat_write_header(fMux, NULL);
		uint8* header = NULL;
		avio_close_dyn_buf(fMux->pb, &header);
		fMux->pb = NULL;
		av_free(header);
		if (result < 0)
			return B_BAD_DATA;
	}
	return B_OK;
}

status_t
AudioEncoder::Write(const float* stereo, size_t frames)
{
	if (fFrameSize == 0)
		return B_NO_INIT;
	while (frames != 0) {
		size_t count = std::min(frames, fFrameSize - fUsed);
		memcpy(fPCM + fUsed * 2, stereo, count * 2 * sizeof(float));
		fUsed += count;
		stereo += count * 2;
		frames -= count;
		if (fUsed == fFrameSize) {
			fUsed = 0;
			status_t status = _Encode();
			if (status != B_OK)
				return status;
		}
	}
	return B_OK;
}

status_t
AudioEncoder::_Encode()
{
	if (fCodec == A2dp::LDAC) {
		uint8 packet[LDACBT_MAX_NBYTES];
		int used = 0, size = 0, frames = 0;
		if (ldacBT_encode(fLdac, fPCM, &used, packet, &size, &frames) != 0
			|| used != int(fFrameSize * 2 * sizeof(float)))
			return B_ERROR;
		return size > 0 ? fWriter(fCookie, packet, size, frames) : B_OK;
	}
	if (av_frame_make_writable(fFrame) < 0)
		return B_NO_MEMORY;
	if (fCodec == A2dp::AAC) {
		for (size_t i = 0; i < fFrameSize; i++) {
			((float*)fFrame->data[0])[i] = fPCM[i * 2];
			((float*)fFrame->data[1])[i] = fPCM[i * 2 + 1];
		}
	} else {
		for (size_t i = 0; i < fFrameSize * 2; i++) {
			float sample = std::isfinite(fPCM[i]) ? fPCM[i] : 0;
			((int16*)fFrame->data[0])[i]
				= std::max(-32768.f, std::min(32767.f, sample * 32768.f));
		}
	}
	fFrame->pts = fSamples;
	fSamples += fFrameSize;
	if (avcodec_send_frame(fContext, fFrame) < 0)
		return B_ERROR;
	int result;
	while ((result = avcodec_receive_packet(fContext, fPacket)) >= 0) {
		status_t status;
		if (fCodec == A2dp::AAC) {
			if (avio_open_dyn_buf(&fMux->pb) < 0) {
				av_packet_unref(fPacket);
				return B_NO_MEMORY;
			}
			av_packet_rescale_ts(fPacket, fContext->time_base,
				fMux->streams[0]->time_base);
			int muxResult = av_write_frame(fMux, fPacket);
			uint8* bytes = NULL;
			int size = avio_close_dyn_buf(fMux->pb, &bytes);
			fMux->pb = NULL;
			// A2DP carries AudioMuxElement directly, without LOAS sync/length.
			status = muxResult >= 0 && size > 3 && bytes[0] == 0x56
				&& (bytes[1] & 0xe0) == 0xe0
				? fWriter(fCookie, bytes + 3, size - 3, 1) : B_BAD_DATA;
			av_free(bytes);
		} else
			status = fWriter(fCookie, fPacket->data, fPacket->size, 1);
		av_packet_unref(fPacket);
		if (status != B_OK)
			return status;
	}
	return result == AVERROR(EAGAIN) ? B_OK : B_ERROR;
}
