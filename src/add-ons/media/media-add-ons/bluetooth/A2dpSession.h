/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef A2DP_SESSION_H
#define A2DP_SESSION_H

#include "AudioEncoder.h"
#include <bluetooth/bluetooth.h>
#include <atomic>

class A2dpSession {
public:
	explicit A2dpSession(const std::atomic<bool>* canceled = NULL);
	~A2dpSession();
	status_t Connect(const bdaddr_t& address, int preferredCodec,
		uint32 bitRate);
	void Disconnect();
	status_t Write(const float* stereo, size_t frames);
	status_t Poll();
	bool IsConnected() const { return fMedia >= 0; }
	const char* CodecName() const;
	const char* Stage() const { return fStage; }

private:
	friend class A2dpSessionTest;
	bool _Canceled() const;
	int _ConnectSocket(const bdaddr_t& address);
	status_t _Transaction(uint8 signal, const uint8* payload, size_t size,
		uint8* reply, size_t& replySize);
	status_t _Receive(uint8& label, uint8& type, uint8& signal,
		uint8* payload, size_t& size);
	status_t _Command(uint8 label, uint8 signal, const uint8* data, size_t size);
	static status_t _Packet(void* cookie, const uint8* bytes, size_t size,
		unsigned frames);
	status_t _SendPacket(const uint8* bytes, size_t size, unsigned frames);

	int fSignal;
	int fMedia;
	uint8 fLabel;
	uint8 fLocalSeid;
	uint16 fMTU;
	uint16 fSequence;
	uint32 fTimestamp;
	uint32 fSSRC;
	bool fStreaming;
	A2dp::Configuration fConfig;
	AudioEncoder fEncoder;
	const std::atomic<bool>* fCanceled;
	const char* fStage;
};
#endif
