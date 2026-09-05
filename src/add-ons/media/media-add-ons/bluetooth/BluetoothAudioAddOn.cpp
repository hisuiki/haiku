/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "BluetoothAudioNode.h"

#include <MediaAddOn.h>
#include <new>
#include <string.h>

class BluetoothAudioAddOn : public BMediaAddOn {
public:
	BluetoothAudioAddOn(image_id image)
		:
		BMediaAddOn(image), fFlavor()
	{
		fFormat = BluetoothAudioNode::Format();
		fFlavor.name = "Bluetooth audio";
		fFlavor.info = "A2DP audio output (SBC, AAC and LDAC)";
		fFlavor.kinds = B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT | B_CONTROLLABLE;
		fFlavor.flavor_flags = B_FLAVOR_IS_GLOBAL;
		fFlavor.possible_count = 1;
		fFlavor.in_format_count = 1;
		fFlavor.in_formats = &fFormat;
	}

	virtual int32 CountFlavors() { return 1; }
	virtual status_t GetFlavorAt(int32 index, const flavor_info** info)
	{
		if (index != 0)
			return B_BAD_INDEX;
		*info = &fFlavor;
		return B_OK;
	}

	virtual BMediaNode* InstantiateNodeFor(const flavor_info* info, BMessage*,
		status_t* error)
	{
		if (info->internal_id != 0) {
			*error = B_BAD_INDEX;
			return NULL;
		}
		BluetoothAudioNode* node = new(std::nothrow) BluetoothAudioNode(this);
		*error = node != NULL ? node->InitCheck() : B_NO_MEMORY;
		if (*error != B_OK) {
			delete node;
			return NULL;
		}
		return node;
	}

private:
	flavor_info fFlavor;
	media_format fFormat;
};

extern "C" _EXPORT BMediaAddOn*
make_media_addon(image_id image)
{
	return new(std::nothrow) BluetoothAudioAddOn(image);
}
