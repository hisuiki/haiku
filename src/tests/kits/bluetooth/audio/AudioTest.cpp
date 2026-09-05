/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "A2dpSession.h"

#include <assert.h>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

struct Capture {
	std::vector<std::vector<uint8> > packets;
	std::vector<unsigned> frames;
	static status_t Write(void* cookie, const uint8* data, size_t size,
		unsigned frames)
	{
		Capture* capture = (Capture*)cookie;
		capture->packets.push_back(std::vector<uint8>(data, data + size));
		capture->frames.push_back(frames);
		return B_OK;
	}
};

class A2dpSessionTest {
public:
	static void Signaling()
	{
		int sockets[2];
		assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
		A2dpSession session;
		session.fSignal = sockets[0];
		uint8 label, type, signal, result[64];
		const uint8 first[] = {0x56, 3, A2dp::GET_CAPABILITIES, 1, 0};
		const uint8 middle[] = {0x5a, 7, 6, 0};
		const uint8 last[] = {0x5e, 0, 0x11, 0x15, 2, 53};
		assert(send(sockets[1], first, sizeof(first), 0) == sizeof(first));
		assert(send(sockets[1], middle, sizeof(middle), 0) == sizeof(middle));
		assert(send(sockets[1], last, sizeof(last), 0) == sizeof(last));
		size_t size = sizeof(result);
		assert(session._Receive(label, type, signal, result, size) == B_OK);
		assert(label == 5 && type == 2 && signal == A2dp::GET_CAPABILITIES);
		A2dp::Configuration config;
		assert(A2dp::SelectConfiguration(result, size, A2dp::SBC, config));
		// A peer command can arrive while our own transaction is pending.
		const uint8 peerCommand[] = {0x30, A2dp::GET_CAPABILITIES, 4};
		const uint8 response[] = {2, A2dp::DISCOVER, 4, 8};
		assert(send(sockets[1], peerCommand, sizeof(peerCommand), 0)
			== sizeof(peerCommand));
		assert(send(sockets[1], response, sizeof(response), 0) == sizeof(response));
		size = sizeof(result);
		assert(session._Transaction(A2dp::DISCOVER, NULL, 0, result, size) == B_OK);
		assert(size == 2 && result[0] == 4 && result[1] == 8);
		assert(recv(sockets[1], result, sizeof(result), 0) == 2);
		assert(result[0] == 0 && result[1] == A2dp::DISCOVER);
		ssize_t count = recv(sockets[1], result, sizeof(result), 0);
		assert(count == 12 && result[0] == 0x32);
		assert(A2dp::SelectConfiguration(result + 2, count - 2, A2dp::SBC, config));
		// A fragment without a START packet is rejected.
		assert(send(sockets[1], last, sizeof(last), 0) == sizeof(last));
		size = sizeof(result);
		assert(session._Receive(label, type, signal, result, size) == B_BAD_DATA);
		close(sockets[1]);
		puts("AVDTP reassembly and interleaved signaling tests passed");
	}

	static void Packets(const Capture& capture, const A2dp::Configuration& config)
	{
		int sockets[2];
		assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
		A2dpSession session;
		session.fMedia = sockets[0];
		session.fMTU = 672;
		session.fConfig = config;
		uint16 sequence = 0;
		uint32 timestamp = 0;
		for (size_t i = 0; i < capture.packets.size(); i++) {
			const std::vector<uint8>& payload = capture.packets[i];
			assert(session._SendPacket(payload.data(), payload.size(),
				capture.frames[i]) == B_OK);
			std::vector<uint8> reconstructed;
			unsigned packetCount = config.codec == A2dp::LDAC ? capture.frames[i]
				: config.codec == A2dp::AAC ? (payload.size() + 659) / 660 : 1;
			for (unsigned n = 0; n < packetCount; n++) {
				uint8 packet[1024];
				ssize_t size = recv(sockets[1], packet, sizeof(packet), 0);
				assert(size > 12 && size <= 672);
				assert(packet[0] == 0x80 && (packet[1] & 0x7f) == 96);
				assert(((packet[2] << 8) | packet[3]) == sequence++);
				uint32 stamp = (uint32(packet[4]) << 24) | (uint32(packet[5]) << 16)
					| (uint32(packet[6]) << 8) | packet[7];
				assert(stamp == timestamp);
				size_t offset = config.codec == A2dp::AAC ? 12 : 13;
				if (config.codec == A2dp::AAC)
					assert(bool(packet[1] & 0x80) == (n + 1 == packetCount));
				else {
					assert(packet[12] == 1);
					timestamp += 128;
				}
				reconstructed.insert(reconstructed.end(), packet + offset, packet + size);
			}
			if (config.codec == A2dp::AAC)
				timestamp += 1024;
			assert(reconstructed == payload);
		}
		close(sockets[1]);
	}
};

struct DecodeEnergy {
	unsigned frames;
	double left;
	double right;
};


static DecodeEnergy
decode(const Capture& capture, A2dp::Codec codec)
{
	const AVCodec* decoder = avcodec_find_decoder(codec == A2dp::AAC
		? AV_CODEC_ID_AAC_LATM : AV_CODEC_ID_SBC);
	assert(decoder != NULL);
	AVCodecContext* context = avcodec_alloc_context3(decoder);
	assert(context != NULL);
	// SBC's decoder expects the sample rate from the negotiated transport.
	context->sample_rate = 48000;
	assert(avcodec_open2(context, decoder, NULL) == 0);
	AVFrame* frame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();
	DecodeEnergy energy = {0, 0, 0};
	for (size_t i = 0; i < capture.packets.size(); i++) {
		const std::vector<uint8>& bytes = capture.packets[i];
		int prefix = codec == A2dp::AAC ? 3 : 0;
		assert(av_new_packet(packet, bytes.size() + prefix) == 0);
		if (prefix) {
			packet->data[0] = 0x56;
			packet->data[1] = 0xe0 | (bytes.size() >> 8);
			packet->data[2] = bytes.size();
		}
		memcpy(packet->data + prefix, bytes.data(), bytes.size());
		assert(avcodec_send_packet(context, packet) == 0);
		int result;
		while ((result = avcodec_receive_frame(context, frame)) == 0) {
			assert(frame->sample_rate == 48000 && frame->ch_layout.nb_channels == 2);
			energy.frames += frame->nb_samples;
			bool planar = frame->format == AV_SAMPLE_FMT_FLTP
				|| frame->format == AV_SAMPLE_FMT_S16P;
			bool floating = frame->format == AV_SAMPLE_FMT_FLT
				|| frame->format == AV_SAMPLE_FMT_FLTP;
			assert(floating || frame->format == AV_SAMPLE_FMT_S16
				|| frame->format == AV_SAMPLE_FMT_S16P);
			for (int n = 0; n < frame->nb_samples; n++) {
				uint8* leftData = frame->data[0];
				uint8* rightData = planar ? frame->data[1] : frame->data[0];
				int leftIndex = planar ? n : n * 2;
				int rightIndex = planar ? n : n * 2 + 1;
				double left = floating
					? ((float*)leftData)[leftIndex]
					: ((int16*)leftData)[leftIndex] / 32768.0;
				double right = floating
					? ((float*)rightData)[rightIndex]
					: ((int16*)rightData)[rightIndex] / 32768.0;
				energy.left += left * left;
				energy.right += right * right;
			}
		}
		assert(result == AVERROR(EAGAIN));
		av_packet_unref(packet);
	}
	assert(energy.frames > 40000 && energy.left / energy.frames > 0.01
		&& energy.right / energy.frames > 0.0005);
	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&context);
	return energy;
}

int
main()
{
	A2dpSessionTest::Signaling();
	const A2dp::Codec codecs[] = {A2dp::SBC, A2dp::AAC, A2dp::LDAC};
	size_t defaultAacBytes = 0;
	for (unsigned c = 0; c < 3; c++) {
		uint8 capabilities[32];
		size_t size = A2dp::MakeCapabilities(codecs[c], capabilities);
		A2dp::Configuration config;
		assert(A2dp::SelectConfiguration(capabilities, size, codecs[c], config));
		Capture capture;
		AudioEncoder encoder;
		assert(encoder.Init(config, 672, Capture::Write, &capture) == B_OK);
		// Deliberately unaligned chunks cover buffering across codec frames.
		float samples[74];
		for (int n = 0; n < 48000; n += 37) {
			for (int i = 0; i < 37; i++) {
				samples[i * 2] = 0.4f * sinf((n + i) * 440.f * 6.2831853f / 48000.f);
				samples[i * 2 + 1]
					= 0.1f * sinf((n + i) * 880.f * 6.2831853f / 48000.f);
			}
			assert(encoder.Write(samples, 37) == B_OK);
		}
		assert(capture.packets.size() > 20);
		if (codecs[c] == A2dp::SBC) {
			assert(capture.packets[0][0] == 0x9c);
			assert(capture.packets[0][1] == 0xfd);
			assert(capture.packets[0][2] == config.data[3]);
		}
		if (codecs[c] == A2dp::AAC) {
			for (size_t p = 0; p < capture.packets.size(); p++) {
				assert(capture.packets[p].size() <= 672 - 12);
				defaultAacBytes += capture.packets[p].size();
			}
		}
		if (codecs[c] != A2dp::LDAC) {
			DecodeEnergy energy = decode(capture, codecs[c]);
			double ratio = energy.left / energy.right;
			assert(ratio > 8 && ratio < 32);
		}
		else {
			for (size_t p = 0; p < capture.packets.size(); p++) {
				const std::vector<uint8>& bytes = capture.packets[p];
				size_t offset = 0;
				for (unsigned f = 0; f < capture.frames[p]; f++) {
					assert(offset + 3 <= bytes.size() && bytes[offset] == 0xaa);
					assert((bytes[offset + 1] >> 5) == 1); // 48 kHz.
					offset += (((bytes[offset + 1] & 7) << 6)
						| (bytes[offset + 2] >> 2)) + 4;
				}
				assert(offset == bytes.size());
			}
		}
		A2dpSessionTest::Packets(capture, config);
		if (codecs[c] == A2dp::AAC) {
			Capture fragmented;
			std::vector<uint8> payload(1700);
			for (size_t n = 0; n < payload.size(); n++)
				payload[n] = n;
			fragmented.packets.push_back(payload);
			fragmented.frames.push_back(1);
			A2dpSessionTest::Packets(fragmented, config);
		}
		printf("%s stereo encode, framing and RTP tests passed\n",
			c == 0 ? "SBC" : c == 1 ? "AAC" : "LDAC");
	}

	uint8 capabilities[32];
	size_t size = A2dp::MakeCapabilities(A2dp::AAC, capabilities);
	A2dp::Configuration lowRateConfig;
	assert(A2dp::SelectConfiguration(capabilities, size, A2dp::AAC,
		lowRateConfig, 64000));
	Capture lowRate;
	AudioEncoder lowRateEncoder;
	assert(lowRateEncoder.Init(lowRateConfig, 672, Capture::Write, &lowRate)
		== B_OK);
	float samples[74];
	for (int n = 0; n < 48000; n += 37) {
		for (int i = 0; i < 37; i++) {
			samples[i * 2]
				= 0.4f * sinf((n + i) * 440.f * 6.2831853f / 48000.f);
			samples[i * 2 + 1] = samples[i * 2];
		}
		assert(lowRateEncoder.Write(samples, 37) == B_OK);
	}
	size_t lowRateBytes = 0;
	for (size_t i = 0; i < lowRate.packets.size(); i++) {
		assert(lowRate.packets[i].size() <= 672 - 12);
		lowRateBytes += lowRate.packets[i].size();
	}
	assert(lowRateBytes < defaultAacBytes);
	decode(lowRate, A2dp::AAC);
	puts("AAC 64 kb/s selection and decode tests passed");
	return 0;
}
