/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_AUDIO_NODE_H
#define BLUETOOTH_AUDIO_NODE_H

#include <BufferConsumer.h>
#include <Controllable.h>
#include <Locker.h>
#include <MediaEventLooper.h>
#include <bluetooth/bluetooth.h>
#include <atomic>

class BluetoothAudioNode : public BBufferConsumer, public BControllable,
	public BMediaEventLooper {
public:
	BluetoothAudioNode(BMediaAddOn* addon);
	virtual ~BluetoothAudioNode();
	status_t InitCheck() const;
	static media_format Format();
	virtual BMediaAddOn* AddOn(int32* id) const;
	virtual status_t HandleMessage(int32 code, const void* data, size_t size);

protected:
	virtual void NodeRegistered();
	virtual status_t AcceptFormat(const media_destination&, media_format*);
	virtual status_t GetNextInput(int32*, media_input*);
	virtual void DisposeInputCookie(int32);
	virtual void BufferReceived(BBuffer*);
	virtual void ProducerDataStatus(const media_destination&, int32, bigtime_t);
	virtual status_t GetLatencyFor(const media_destination&, bigtime_t*,
		media_node_id*);
	virtual status_t Connected(const media_source&, const media_destination&,
		const media_format&, media_input*);
	virtual void Disconnected(const media_source&, const media_destination&);
	virtual status_t FormatChanged(const media_source&, const media_destination&,
		int32, const media_format&);
	virtual void HandleEvent(const media_timed_event*, bigtime_t, bool);
	virtual status_t GetParameterValue(int32, bigtime_t*, void*, size_t*);
	virtual void SetParameterValue(int32, bigtime_t, const void*, size_t);

private:
	static int32 _WorkerEntry(void* cookie);
	int32 _Worker();
	void _LoadSettings();
	void _SaveSettings();

	BMediaAddOn* fAddOn;
	media_input fInput;
	port_id fPort;
	thread_id fWorker;
	BLocker fLock;
	float fVolume;
	int32 fMute;
	int32 fPreferredCodec;
	int32 fBitRate;
	bigtime_t fChanged;
	bdaddr_t fAddress;
	std::atomic<bool> fStopping;
};
#endif
