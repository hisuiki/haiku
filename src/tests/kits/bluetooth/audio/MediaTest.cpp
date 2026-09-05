/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "BluetoothAudioNode.h"

#include <Application.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char** argv)
{
	BApplication application(argc == 2
		&& strcmp(argv[1], "--select-installed") == 0
		? "application/x-vnd.Haiku-bluetooth-audio-selector"
		: "application/x-vnd.Haiku-bluetooth-audio-test");
	status_t status;
	BMediaRoster* roster = BMediaRoster::Roster(&status);
	assert(roster != NULL && status == B_OK);
	if (argc == 2 && strcmp(argv[1], "--select-installed") == 0) {
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		live_node_info nodes[64];
		int32 count = 64;
		status = roster->GetLiveNodes(nodes, &count, &format, NULL, NULL,
			B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT);
		if (status != B_OK) {
			fprintf(stderr, "GetLiveNodes failed: %s\n", strerror(status));
			return 1;
		}
		for (int32 i = 0; i < count; i++) {
			if (strcmp(nodes[i].name, "Bluetooth audio") != 0)
				continue;
			BParameterWeb* web = NULL;
			status = roster->GetParameterWebFor(nodes[i].node, &web);
			if (status != B_OK || web == NULL) {
				fprintf(stderr, "GetParameterWebFor failed: %s\n", strerror(status));
				return 1;
			}
			bool setBitRate = false;
			for (int32 parameterIndex = 0;
					parameterIndex < web->CountParameters(); parameterIndex++) {
				BParameter* parameter = web->ParameterAt(parameterIndex);
				if (parameter->ID() == 4) {
					int32 bitRate = 128000;
					status = parameter->SetValue(&bitRate, sizeof(bitRate), 0);
					setBitRate = status == B_OK;
					break;
				}
			}
			delete web;
			if (!setBitRate) {
				fprintf(stderr, "Could not set installed bitrate control\n");
				return 1;
			}
			status = roster->SetAudioOutput(nodes[i].node);
			if (status != B_OK) {
				fprintf(stderr, "SetAudioOutput failed: %s (%" B_PRId32 ")\n",
					strerror(status), status);
				return 1;
			}
			media_node selected;
			status = roster->GetAudioOutput(&selected);
			if (status != B_OK || selected.node != nodes[i].node.node) {
				fprintf(stderr, "GetAudioOutput verification failed: %s\n",
					strerror(status));
				return 1;
			}
			roster->ReleaseNode(selected);
			for (int retry = 0; retry < 50; retry++) {
				media_input inputs[8];
				int32 connected = 8;
				status = roster->GetConnectedInputsFor(nodes[i].node, inputs,
					8, &connected);
				if (status == B_OK && connected > 0) {
					printf("Bluetooth audio selected and connected: %.0f Hz, "
						"%" B_PRIu32 " channels, format %#" B_PRIx32 "\n",
						inputs[0].format.u.raw_audio.frame_rate,
						inputs[0].format.u.raw_audio.channel_count,
						inputs[0].format.u.raw_audio.format);
					return 0;
				}
				snooze(100000);
			}
			fprintf(stderr, "Bluetooth audio selected, but mixer did not connect\n");
			return 1;
		}
		fprintf(stderr, "Bluetooth audio live node not found\n");
		return 1;
	}
	BluetoothAudioNode* node = new BluetoothAudioNode(NULL);
	assert(node->InitCheck() == B_OK);
	assert(roster->RegisterNode(node) == B_OK);
	assert((node->Node().kind & (B_PHYSICAL_OUTPUT | B_CONTROLLABLE))
		== (B_PHYSICAL_OUTPUT | B_CONTROLLABLE));
	media_input input;
	int32 count = 1;
	assert(roster->GetFreeInputsFor(node->Node(), &input, 1, &count,
		B_MEDIA_RAW_AUDIO) == B_OK && count == 1);
	assert(input.format.u.raw_audio.frame_rate == 48000);
	assert(input.format.u.raw_audio.channel_count == 2);
	BParameterWeb* web = NULL;
	assert(roster->GetParameterWebFor(node->Node(), &web) == B_OK);
	assert(web != NULL && web->CountParameters() == 4);
	bool foundBitRate = false;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		bigtime_t changed;
		uint32 original;
		size_t size = sizeof(original);
		assert(parameter->GetValue(&original, &size, &changed) == B_OK);
		assert(size == sizeof(original));
		if (parameter->ID() == 1) {
			float volume = -12;
			assert(parameter->SetValue(&volume, sizeof(volume), 0) == B_OK);
			volume = 0;
			assert(parameter->GetValue(&volume, &size, &changed) == B_OK);
			assert(volume == -12);
		}
		if (parameter->ID() == 4) {
			foundBitRate = true;
			int32 bitRate = 256000;
			assert(parameter->SetValue(&bitRate, sizeof(bitRate), 0) == B_OK);
			bitRate = 0;
			size = sizeof(bitRate);
			assert(parameter->GetValue(&bitRate, &size, &changed) == B_OK);
			assert(bitRate == 256000);
		}
		assert(parameter->SetValue(&original, sizeof(original), 0) == B_OK);
	}
	assert(foundBitRate);
	delete web;
	node->Release();
	puts("Bluetooth media output registration and mixer control tests passed");
	return 0;
}
