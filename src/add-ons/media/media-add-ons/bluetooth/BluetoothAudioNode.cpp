/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "BluetoothAudioNode.h"
#include "A2dpSession.h"

#include <Autolock.h>
#include <BluetoothAudio.h>
#include <Buffer.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <MediaAddOn.h>
#include <Message.h>
#include <ParameterWeb.h>
#include <Path.h>
#include <TimeSource.h>

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>

enum { kVolume = 1, kMute, kCodec, kBitRate };
static const int32 kAudioBuffer = 'bpcm';
static const bigtime_t kLatency = 100000;

static bool
IsBitRate(int32 value)
{
	return value == 64000 || value == 96000 || value == 128000
		|| value == 256000 || value == 330000 || value == 660000
		|| value == 990000;
}

struct AudioChunk {
	bigtime_t queued;
	float samples[1024];
};

BluetoothAudioNode::BluetoothAudioNode(BMediaAddOn* addon)
	:
	BMediaNode("Bluetooth audio"), BBufferConsumer(B_MEDIA_RAW_AUDIO),
	BControllable(), BMediaEventLooper(), fAddOn(addon), fPort(-1),
	fWorker(-1), fLock("Bluetooth audio settings"), fVolume(0), fMute(0),
	fPreferredCodec(-1), fBitRate(A2dp::kDefaultBitRate), fChanged(0),
	fStopping(false)
{
	AddNodeKind(B_PHYSICAL_OUTPUT);
	memset(&fAddress, 0, sizeof(fAddress));
	_LoadSettings();
	fInput.source = media_source::null;
	fInput.destination = media_destination::null;
	fInput.format = Format();
	strlcpy(fInput.name, "Bluetooth audio", sizeof(fInput.name));
	fPort = create_port(8, BLUETOOTH_AUDIO_PORT);
}

BluetoothAudioNode::~BluetoothAudioNode()
{
	fStopping.store(true);
	Quit();
	if (fPort >= B_OK)
		delete_port(fPort);
	if (fWorker >= B_OK) {
		status_t result;
		wait_for_thread(fWorker, &result);
	}
	_SaveSettings();
}

status_t
BluetoothAudioNode::InitCheck() const
{
	return fPort < B_OK ? fPort : B_OK;
}

media_format
BluetoothAudioNode::Format()
{
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	format.u.raw_audio.frame_rate = 48000;
	format.u.raw_audio.channel_count = 2;
	format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	return format;
}

BMediaAddOn*
BluetoothAudioNode::AddOn(int32* id) const
{
	*id = 0;
	return fAddOn;
}

void
BluetoothAudioNode::NodeRegistered()
{
	fInput.node = Node();
	fInput.destination = media_destination(ControlPort(), 0);
	BParameterWeb* web = new BParameterWeb;
	BParameterGroup* group = web->MakeGroup("Bluetooth audio");
	group->MakeContinuousParameter(kVolume, B_MEDIA_RAW_AUDIO, "Volume",
		B_MASTER_GAIN, "dB", -60, 0, 1);
	BDiscreteParameter* mute = group->MakeDiscreteParameter(kMute,
		B_MEDIA_RAW_AUDIO, "Mute", B_MUTE);
	mute->AddItem(0, "Off");
	mute->AddItem(1, "On");
	BDiscreteParameter* codec = group->MakeDiscreteParameter(kCodec,
		B_MEDIA_RAW_AUDIO, "Codec (next connection)", B_GENERIC);
	codec->AddItem(-1, "Automatic (LDAC, AAC, SBC)");
	codec->AddItem(A2dp::SBC, "SBC");
	codec->AddItem(A2dp::AAC, "AAC");
	codec->AddItem(A2dp::LDAC, "LDAC");
	BDiscreteParameter* bitRate = group->MakeDiscreteParameter(kBitRate,
		B_MEDIA_RAW_AUDIO, "Bitrate (next connection)", B_GENERIC);
	bitRate->AddItem(64000, "64 kb/s");
	bitRate->AddItem(96000, "96 kb/s");
	bitRate->AddItem(128000, "128 kb/s");
	bitRate->AddItem(256000, "256 kb/s");
	bitRate->AddItem(330000, "330 kb/s");
	bitRate->AddItem(660000, "660 kb/s");
	bitRate->AddItem(990000, "990 kb/s");
	SetParameterWeb(web);
	SetEventLatency(kLatency);
	fWorker = spawn_thread(_WorkerEntry, "Bluetooth audio transport",
		B_NORMAL_PRIORITY, this);
	if (fWorker >= B_OK)
		resume_thread(fWorker);
	Run();
}

status_t
BluetoothAudioNode::HandleMessage(int32 code, const void* data, size_t size)
{
	if (BBufferConsumer::HandleMessage(code, data, size) == B_OK
		|| BControllable::HandleMessage(code, data, size) == B_OK
		|| BMediaNode::HandleMessage(code, data, size) == B_OK)
		return B_OK;
	return B_ERROR;
}

status_t
BluetoothAudioNode::AcceptFormat(const media_destination& destination,
	media_format* format)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	media_format expected = Format();
	bool compatible = format->Matches(&expected);
	size_t bufferSize = format->type == B_MEDIA_RAW_AUDIO
		? format->u.raw_audio.buffer_size : 0;
	*format = expected;
	format->u.raw_audio.buffer_size = bufferSize != 0 ? bufferSize : 4096;
	return compatible ? B_OK : B_MEDIA_BAD_FORMAT;
}

status_t
BluetoothAudioNode::GetNextInput(int32* cookie, media_input* input)
{
	if ((*cookie)++ != 0)
		return B_BAD_INDEX;
	*input = fInput;
	return B_OK;
}

void BluetoothAudioNode::DisposeInputCookie(int32) {}
void BluetoothAudioNode::ProducerDataStatus(const media_destination&, int32,
	bigtime_t) {}

status_t
BluetoothAudioNode::GetLatencyFor(const media_destination& destination,
	bigtime_t* latency, media_node_id* timeSource)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	*latency = kLatency;
	*timeSource = TimeSource()->ID();
	return B_OK;
}

status_t
BluetoothAudioNode::Connected(const media_source& source,
	const media_destination& destination, const media_format& format,
	media_input* input)
{
	if (fInput.source != media_source::null)
		return B_MEDIA_ALREADY_CONNECTED;
	media_format accepted = format;
	status_t status = AcceptFormat(destination, &accepted);
	if (status != B_OK)
		return status;
	fInput.source = source;
	fInput.format = accepted;
	*input = fInput;
	return B_OK;
}

void
BluetoothAudioNode::Disconnected(const media_source& source,
	const media_destination& destination)
{
	if (fInput.source == source && fInput.destination == destination) {
		fInput.source = media_source::null;
		EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
			BTimedEventQueue::B_HANDLE_BUFFER);
	}
}

status_t
BluetoothAudioNode::FormatChanged(const media_source& source,
	const media_destination& destination, int32, const media_format& format)
{
	if (source != fInput.source)
		return B_MEDIA_BAD_SOURCE;
	media_format accepted = format;
	status_t status = AcceptFormat(destination, &accepted);
	if (status == B_OK)
		fInput.format = accepted;
	return status;
}

void
BluetoothAudioNode::BufferReceived(BBuffer* buffer)
{
	if (buffer->Header()->destination != fInput.destination.id
		|| buffer->SizeUsed() % (2 * sizeof(float)) != 0) {
		buffer->Recycle();
		return;
	}
	media_timed_event event(buffer->Header()->start_time,
		BTimedEventQueue::B_HANDLE_BUFFER, buffer, BTimedEventQueue::B_RECYCLE_BUFFER);
	if (EventQueue()->AddEvent(event) != B_OK)
		buffer->Recycle();
}

void
BluetoothAudioNode::HandleEvent(const media_timed_event* event, bigtime_t,
	bool)
{
	if (event->type == BTimedEventQueue::B_STOP) {
		EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
			BTimedEventQueue::B_HANDLE_BUFFER);
		return;
	}
	if (event->type != BTimedEventQueue::B_HANDLE_BUFFER)
		return;
	BBuffer* buffer = (BBuffer*)event->pointer;
	if (RunState() == B_STARTED) {
		float gain;
		{
			BAutolock lock(&fLock);
			gain = fMute ? 0 : powf(10.f, fVolume / 20.f);
		}
		const float* samples = (const float*)buffer->Data();
		size_t remaining = buffer->SizeUsed() / sizeof(float);
		while (remaining != 0) {
			AudioChunk chunk;
			chunk.queued = system_time();
			size_t count = std::min<size_t>(remaining, 1024);
			for (size_t i = 0; i < count; i++) {
				float sample = std::isfinite(samples[i]) ? samples[i] : 0;
				chunk.samples[i] = std::max(-1.f, std::min(1.f, sample * gain));
			}
			// Never block the media event thread behind radio I/O.
			if (write_port_etc(fPort, kAudioBuffer, &chunk,
					sizeof(chunk.queued) + count * sizeof(float),
					B_RELATIVE_TIMEOUT, 0) != B_OK)
				break;
			samples += count;
			remaining -= count;
		}
	}
	buffer->Recycle();
}

status_t
BluetoothAudioNode::GetParameterValue(int32 id, bigtime_t* changed, void* value,
	size_t* size)
{
	if (*size < sizeof(int32))
		return B_NO_MEMORY;
	BAutolock lock(&fLock);
	const void* source;
	switch (id) {
		case kVolume: source = &fVolume; break;
		case kMute: source = &fMute; break;
		case kCodec: source = &fPreferredCodec; break;
		case kBitRate: source = &fBitRate; break;
		default: return B_BAD_VALUE;
	}
	memcpy(value, source, sizeof(int32));
	*size = sizeof(int32);
	*changed = fChanged;
	return B_OK;
}

void
BluetoothAudioNode::SetParameterValue(int32 id, bigtime_t when,
	const void* value, size_t size)
{
	if (size != sizeof(int32))
		return;
	BAutolock lock(&fLock);
	int32 discrete;
	memcpy(&discrete, value, sizeof(discrete));
	switch (id) {
		case kVolume:
		{
			float volume;
			memcpy(&volume, value, sizeof(volume));
			if (!std::isfinite(volume) || volume < -60 || volume > 0)
				return;
			fVolume = volume;
			break;
		}
		case kMute:
			if (discrete != 0 && discrete != 1) return;
			fMute = discrete;
			break;
		case kCodec:
			if (discrete != -1 && discrete != A2dp::SBC
				&& discrete != A2dp::AAC && discrete != A2dp::LDAC) return;
			fPreferredCodec = discrete;
			break;
		case kBitRate:
			if (!IsBitRate(discrete)) return;
			fBitRate = discrete;
			break;
		default: return;
	}
	fChanged = when;
	BroadcastNewParameterValue(when, id, const_cast<void*>(value), size);
}

int32
BluetoothAudioNode::_WorkerEntry(void* cookie)
{
	return ((BluetoothAudioNode*)cookie)->_Worker();
}

int32
BluetoothAudioNode::_Worker()
{
	A2dpSession session(&fStopping);
	AudioChunk chunk;
	while (!fStopping.load()) {
		int32 code;
		ssize_t size = read_port_etc(fPort, &code, &chunk, sizeof(chunk),
			B_RELATIVE_TIMEOUT, 20000);
		if (size == B_BAD_PORT_ID)
			break;
		status_t status = B_OK;
		int preferredCodec, bitRate;
		{
			BAutolock lock(&fLock);
			preferredCodec = fPreferredCodec;
			bitRate = fBitRate;
		}
		if (size == sizeof(bluetooth_audio_connect)
			&& code == BLUETOOTH_AUDIO_CONNECT) {
			bluetooth_audio_connect request;
			memcpy(&request, &chunk, sizeof(request));
			status = session.Connect(request.address, preferredCodec, bitRate);
			if (status == B_OK) {
				{
					BAutolock lock(&fLock);
					fAddress = request.address;
				}
				printf("Bluetooth audio: connected using %s\n", session.CodecName());
			}
		} else if (size == sizeof(bluetooth_audio_connect)
			&& code == BLUETOOTH_AUDIO_DISCONNECT) {
			BAutolock lock(&fLock);
			if (memcmp(&fAddress, &chunk, sizeof(fAddress)) == 0)
				session.Disconnect();
		} else if (size > ssize_t(sizeof(chunk.queued)) && code == kAudioBuffer
			&& (size - sizeof(chunk.queued)) % (2 * sizeof(float)) == 0) {
			if (system_time() - chunk.queued < kLatency)
				status = session.Write(chunk.samples,
					(size - sizeof(chunk.queued)) / (2 * sizeof(float)));
		}
		if (status == B_OK)
			status = session.Poll();
		if (status != B_OK) {
			fprintf(stderr, "Bluetooth audio: %s: %s\n", session.Stage(),
				strerror(status));
			session.Disconnect();
		}
	}
	return B_OK;
}

void
BluetoothAudioNode::_LoadSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("Media/bluetooth_audio_settings");
	BFile file(path.Path(), B_READ_ONLY);
	BMessage settings;
	if (settings.Unflatten(&file) != B_OK)
		return;
	float volume;
	int32 value;
	if (settings.FindFloat("volume", &volume) == B_OK
		&& std::isfinite(volume) && volume >= -60 && volume <= 0)
		fVolume = volume;
	if (settings.FindInt32("mute", &value) == B_OK && (value == 0 || value == 1))
		fMute = value;
	if (settings.FindInt32("codec", &value) == B_OK
		&& (value == -1 || value == 0 || value == 2 || value == 255))
		fPreferredCodec = value;
	if (settings.FindInt32("bitrate", &value) == B_OK && IsBitRate(value))
		fBitRate = value;
	else if (settings.FindInt32("aac bitrate", &value) == B_OK
		&& IsBitRate(value))
		fBitRate = value;
}

void
BluetoothAudioNode::_SaveSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("Media");
	create_directory(path.Path(), 0755);
	path.Append("bluetooth_audio_settings");
	BMessage settings;
	settings.AddFloat("volume", fVolume);
	settings.AddInt32("mute", fMute);
	settings.AddInt32("codec", fPreferredCodec);
	settings.AddInt32("bitrate", fBitRate);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		settings.Flatten(&file);
}
