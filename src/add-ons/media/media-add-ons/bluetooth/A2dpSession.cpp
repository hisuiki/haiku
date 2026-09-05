/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "A2dpSession.h"

#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/l2cap.h>
#include <OS.h>
#include <algorithm>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace A2dp;

A2dpSession::A2dpSession(const std::atomic<bool>* canceled)
	:
	fSignal(-1), fMedia(-1), fLabel(0), fLocalSeid(0), fMTU(0),
	fSequence(0), fTimestamp(0), fSSRC(uint32(system_time())), fStreaming(false),
	fCanceled(canceled), fStage("idle")
{
	memset(&fConfig, 0, sizeof(fConfig));
}

bool
A2dpSession::_Canceled() const
{
	return fCanceled != NULL && fCanceled->load();
}

A2dpSession::~A2dpSession()
{
	Disconnect();
}

void
A2dpSession::Disconnect()
{
	// Closing both channels also tears down a partially configured stream.
	if (fMedia >= 0)
		close(fMedia);
	if (fSignal >= 0)
		close(fSignal);
	fMedia = fSignal = -1;
	fStreaming = false;
	fLocalSeid = 0;
	fEncoder.Reset();
}

int
A2dpSession::_ConnectSocket(const bdaddr_t& address)
{
	if (_Canceled())
		return -1;
	int fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return -1;
	timeval timeout = {5, 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	sockaddr_l2cap peer = {};
	peer.l2cap_len = sizeof(peer);
	peer.l2cap_family = AF_BLUETOOTH;
	peer.l2cap_psm = L2CAP_PSM_AVDTP;
	peer.l2cap_bdaddr = address;
	if (connect(fd, (sockaddr*)&peer, sizeof(peer)) < 0) {
		fprintf(stderr, "Bluetooth audio: %s: %s\n", fStage, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

status_t
A2dpSession::Connect(const bdaddr_t& address, int preferredCodec,
	uint32 bitRate)
{
	Disconnect();
	fStage = "connect AVDTP signaling";
	fSignal = _ConnectSocket(address);
	if (fSignal < 0)
		return B_ERROR;
	fStage = "discover audio endpoints";
	uint8 endpoints[128];
	size_t size = sizeof(endpoints);
	status_t status = _Transaction(DISCOVER, NULL, 0, endpoints, size);
	if (status != B_OK || size % 2 != 0) {
		Disconnect();
		return status != B_OK ? status : B_BAD_DATA;
	}
	const Codec preference[] = {LDAC, AAC, SBC};
	bigtime_t deadline = system_time() + 20000000;
	bool configured = false;
	for (size_t c = 0; c < 3 && !configured; c++) {
		Codec codec = preference[c];
		if (preferredCodec >= 0 && codec != preferredCodec)
			continue;
		for (size_t i = 0; i < size; i += 2) {
			if (_Canceled() || system_time() >= deadline) {
				Disconnect();
				return _Canceled() ? B_CANCELED : B_TIMED_OUT;
			}
			uint8 seid = endpoints[i] >> 2;
			// Skip in-use endpoints, non-audio endpoints and sources.
			if ((endpoints[i] & 2) || seid == 0 || seid > 62
				|| (endpoints[i + 1] & 0xf8) != 8)
				continue;
			uint8 target = seid << 2;
			uint8 capabilities[512];
			size_t count = sizeof(capabilities);
			fStage = "read codec capabilities";
			if (_Transaction(GET_CAPABILITIES, &target, 1, capabilities,
					count) != B_OK)
				continue;
			Configuration config;
			if (!SelectConfiguration(capabilities, count, codec, config,
				bitRate))
				continue;
			config.seid = seid;
			// Probe the encoder before committing to a remote configuration.
			fStage = "initialize codec";
			if (fEncoder.Init(config, L2CAP_MTU_DEFAULT, _Packet,
					this) != B_OK)
				continue;
			fLocalSeid = codec == SBC ? 1 : codec == AAC ? 2 : 3;
			uint8 request[16] = {target, uint8(fLocalSeid << 2), 1, 0, 7,
				uint8(config.size + 2), 0, uint8(codec)};
			memcpy(request + 8, config.data, config.size);
			count = 0;
			fStage = "configure remote endpoint";
			if (_Transaction(SET_CONFIGURATION, request, config.size + 8,
					NULL, count) != B_OK)
				continue;
			fConfig = config;
			configured = true;
			break;
		}
	}
	if (!configured) {
		fStage = "find compatible 48 kHz stereo codec";
		Disconnect();
		return B_NOT_SUPPORTED;
	}
	uint8 target = fConfig.seid << 2;
	size = 0;
	fStage = "open remote endpoint";
	status = _Transaction(OPEN, &target, 1, NULL, size);
	if (status == B_OK) {
		fStage = "connect AVDTP media channel";
		fMedia = _ConnectSocket(address);
		if (fMedia < 0)
			status = B_ERROR;
	}
	if (status == B_OK) {
		socklen_t length = sizeof(fMTU);
		fStage = "configure encoder for media MTU";
		if (getsockopt(fMedia, BLUETOOTH_PROTO_L2CAP, SO_L2CAP_OUTGOING_MTU,
				&fMTU, &length) < 0 || length != sizeof(fMTU) || fMTU < 48)
			status = B_ERROR;
		else {
			fMTU = std::min<uint16>(fMTU, 4096);
			status = fEncoder.Init(fConfig, fMTU, _Packet, this);
		}
	}
	if (status == B_OK) {
		size = 0;
		fStage = "start remote stream";
		status = _Transaction(START, &target, 1, NULL, size);
	}
	if (status != B_OK) {
		Disconnect();
		return status;
	}
	// A stalled radio must not stall the media node or grow an audio backlog.
	timeval timeout = {0, 100000};
	setsockopt(fMedia, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	fStreaming = true;
	fSequence = 0;
	fTimestamp = 0;
	fStage = "stream audio";
	return B_OK;
}

status_t
A2dpSession::_Receive(uint8& label, uint8& type, uint8& signal,
	uint8* payload, size_t& size)
{
	uint8 packet[1024];
	size_t capacity = size;
	size = 0;
	unsigned remaining = 0;
	bigtime_t deadline = system_time() + 5000000;
	do {
		for (;;) {
			if (_Canceled())
				return B_CANCELED;
			if (system_time() >= deadline)
				return B_TIMED_OUT;
			pollfd fd = {fSignal, POLLIN, 0};
			int ready = poll(&fd, 1, 100);
			if (ready < 0 && errno != EINTR)
				return B_IO_ERROR;
			if (ready > 0) {
				if (fd.revents & (POLLHUP | POLLERR | POLLNVAL))
					return B_IO_ERROR;
				break;
			}
		}
		ssize_t count = recv(fSignal, packet, sizeof(packet), 0);
		if (count < 2)
			return B_IO_ERROR;
		uint8 packetType = (packet[0] >> 2) & 3;
		size_t offset;
		if (remaining == 0) {
			label = packet[0] >> 4;
			type = packet[0] & 3;
			if (packetType == 0) {
				signal = packet[1] & 0x3f;
				offset = 2;
			} else if (packetType == 1 && count >= 3 && packet[1] >= 2) {
				remaining = packet[1] - 1;
				signal = packet[2] & 0x3f;
				offset = 3;
			} else
				return B_BAD_DATA;
		} else {
			if ((packet[0] >> 4) != label || (packet[0] & 3) != type
				|| packetType != (remaining == 1 ? 3 : 2))
				return B_BAD_DATA;
			remaining--;
			offset = 1;
		}
		if (size_t(count) - offset > capacity - size)
			return B_BUFFER_OVERFLOW;
		memcpy(payload + size, packet + offset, count - offset);
		size += count - offset;
	} while (remaining != 0);
	return B_OK;
}

status_t
A2dpSession::_Transaction(uint8 signal, const uint8* payload, size_t size,
	uint8* reply, size_t& replySize)
{
	uint8 packet[1024];
	if (size > sizeof(packet) - 2)
		return B_BAD_VALUE;
	uint8 label = fLabel++ & 15;
	packet[0] = label << 4;
	packet[1] = signal;
	if (size != 0)
		memcpy(packet + 2, payload, size);
	if (send(fSignal, packet, size + 2, 0) != ssize_t(size + 2))
		return B_IO_ERROR;
	bigtime_t deadline = system_time() + 5000000;
	while (system_time() < deadline) {
		uint8 receivedLabel, type, receivedSignal;
		size_t count = sizeof(packet);
		status_t status = _Receive(receivedLabel, type, receivedSignal,
			packet, count);
		if (status != B_OK)
			return status;
		if (type == 0) {
			status = _Command(receivedLabel, receivedSignal, packet, count);
			if (status != B_OK)
				return status;
			continue;
		}
		if (receivedLabel != label || receivedSignal != signal)
			continue;
		if (type != 2) {
			fprintf(stderr, "Bluetooth audio: %s rejected, signal %#x, error %#x\n",
				fStage, signal, count != 0 ? packet[count - 1] : 0);
			return B_NOT_ALLOWED;
		}
		if (count > replySize)
			return B_BUFFER_OVERFLOW;
		if (count != 0)
			memcpy(reply, packet, count);
		replySize = count;
		return B_OK;
	}
	return B_TIMED_OUT;
}

status_t
A2dpSession::_Command(uint8 label, uint8 signal, const uint8* data, size_t size)
{
	uint8 reply[64] = {uint8((label << 4) | 2), signal};
	size_t length = 2;
	uint8 error = 0;
	bool closeStream = false;
	if (signal == DISCOVER && size == 0) {
		for (int i = 1; i <= 3; i++) {
			reply[length++] = (i << 2) | (fLocalSeid == i ? 2 : 0);
			reply[length++] = 0; // Audio source.
		}
	} else if ((signal == GET_CAPABILITIES || signal == GET_ALL_CAPABILITIES)
		&& size == 1 && (data[0] >> 2) >= 1 && (data[0] >> 2) <= 3) {
		uint8 seid = data[0] >> 2;
		length += MakeCapabilities(seid == 1 ? SBC : seid == 2 ? AAC : LDAC,
			reply + length);
	} else if (signal == GET_CONFIGURATION && size == 1 && fLocalSeid != 0
		&& (data[0] >> 2) == fLocalSeid) {
		reply[length++] = 1;
		reply[length++] = 0;
		reply[length++] = 7;
		reply[length++] = fConfig.size + 2;
		reply[length++] = 0;
		reply[length++] = fConfig.codec;
		memcpy(reply + length, fConfig.data, fConfig.size);
		length += fConfig.size;
	} else if ((signal == START || signal == SUSPEND || signal == CLOSE
			|| signal == ABORT) && size == 1 && fLocalSeid != 0
		&& (data[0] >> 2) == fLocalSeid) {
		if (signal == START) {
			if (fMedia < 0)
				error = 0x31; // BAD_STATE
			else
				fStreaming = true;
		} else {
			fStreaming = false;
			closeStream = signal == CLOSE || signal == ABORT;
		}
	} else
		error = 0x19; // NOT_SUPPORTED_COMMAND
	if (error != 0) {
		reply[0] = (label << 4) | 3;
		if (signal == START || signal == SUSPEND)
			reply[length++] = size > 0 ? data[0] : 0;
		if (signal == SET_CONFIGURATION || signal == RECONFIGURE)
			reply[length++] = 0;
		reply[length++] = error;
	}
	if (send(fSignal, reply, length, 0) != ssize_t(length))
		return B_IO_ERROR;
	return closeStream ? B_CANCELED : B_OK;
}

status_t
A2dpSession::Poll()
{
	if (fSignal < 0)
		return B_OK;
	pollfd fd = {fSignal, POLLIN, 0};
	int ready = poll(&fd, 1, 0);
	if (ready < 0)
		return errno == EINTR ? B_OK : B_IO_ERROR;
	if (ready == 0)
		return B_OK;
	if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return B_IO_ERROR;
	uint8 label, type, signal, packet[1024];
	size_t size = sizeof(packet);
	status_t status = _Receive(label, type, signal, packet, size);
	if (status == B_OK && type == 0)
		status = _Command(label, signal, packet, size);
	return status;
}

status_t
A2dpSession::Write(const float* stereo, size_t frames)
{
	if (fMedia < 0 || !fStreaming)
		return B_OK;
	return fEncoder.Write(stereo, frames);
}

const char*
A2dpSession::CodecName() const
{
	return fConfig.codec == LDAC ? "LDAC" : fConfig.codec == AAC ? "AAC" : "SBC";
}

status_t
A2dpSession::_Packet(void* cookie, const uint8* bytes, size_t size,
	unsigned frames)
{
	return ((A2dpSession*)cookie)->_SendPacket(bytes, size, frames);
}

status_t
A2dpSession::_SendPacket(const uint8* bytes, size_t size, unsigned frames)
{
	if (fConfig.codec == LDAC && frames > 1) {
		// libldac returns a batch. Each self-delimiting frame can be carried
		// in its own RTP packet, avoiding fragmentation on a 672-byte MTU.
		for (unsigned i = 0; i < frames; i++) {
			if (size < 3 || bytes[0] != 0xaa)
				return B_BAD_DATA;
			size_t length = (((bytes[1] & 7) << 6) | (bytes[2] >> 2)) + 4;
			if (length > size)
				return B_BAD_DATA;
			status_t status = _SendPacket(bytes, length, 1);
			if (status != B_OK)
				return status;
			bytes += length;
			size -= length;
		}
		return size == 0 ? B_OK : B_BAD_DATA;
	}
	uint8 packet[4096];
	bool aac = fConfig.codec == AAC;
	size_t headerSize = aac ? 12 : 13;
	if (frames == 0 || frames > 15 || fMTU <= headerSize)
		return B_BAD_DATA;
	if (!aac && size > fMTU - headerSize)
		return B_BUFFER_OVERFLOW;
	while (size != 0) {
		size_t count = std::min(size, fMTU - headerSize);
		MakeRtpHeader(packet, fSequence++, fTimestamp, fSSRC,
			aac && count == size);
		if (!aac)
			packet[12] = frames;
		memcpy(packet + headerSize, bytes, count);
		if (send(fMedia, packet, headerSize + count, 0)
				!= ssize_t(headerSize + count))
			return B_IO_ERROR;
		bytes += count;
		size -= count;
	}
	fTimestamp += frames * (aac ? 1024 : 128);
	return B_OK;
}
