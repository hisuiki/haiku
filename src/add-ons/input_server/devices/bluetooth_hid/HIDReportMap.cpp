/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	A small HID report descriptor reader.

	A Low Energy peripheral that offers no boot protocol mode describes its
	own report layout instead. Only what a keyboard or a mouse needs is kept:
	the buttons, the pointer axes, and the keyboard modifier and key fields.
*/

#include "HIDReportMap.h"

#include <string.h>


// Item prefix, HID specification 6.2.2.2.
#define HID_ITEM_TYPE_MAIN		0
#define HID_ITEM_TYPE_GLOBAL	1
#define HID_ITEM_TYPE_LOCAL		2

#define HID_MAIN_INPUT			0x08
#define HID_MAIN_COLLECTION		0x0a
#define HID_MAIN_END_COLLECTION	0x0c

#define HID_GLOBAL_USAGE_PAGE	0x00
#define HID_GLOBAL_REPORT_SIZE	0x07
#define HID_GLOBAL_REPORT_ID	0x08
#define HID_GLOBAL_REPORT_COUNT	0x09

#define HID_LOCAL_USAGE			0x00
#define HID_LOCAL_USAGE_MINIMUM	0x01
#define HID_LOCAL_USAGE_MAXIMUM	0x02

// Input item data bits, HID specification 6.2.2.5.
#define HID_INPUT_CONSTANT		0x01
#define HID_INPUT_VARIABLE		0x02
#define HID_INPUT_RELATIVE		0x04

// Every report id gets its own run of bit offsets.
#define HID_MAX_REPORT_IDS		16


HIDReportMap::HIDReportMap()
	:
	fItemCount(0)
{
	memset(fItems, 0, sizeof(fItems));
}


bool
HIDReportMap::Parse(const uint8* descriptor, size_t size)
{
	fItemCount = 0;

	uint16 usagePage = 0;
	uint8 reportSize = 0;
	uint8 reportCount = 0;
	uint8 reportId = 0;

	uint16 namedUsages[HID_MAX_NAMED_USAGES];
	uint8 namedUsageCount = 0;
	uint16 usageMinimum = 0;
	uint16 usageMaximum = 0;

	// Input reports are laid out independently of one another.
	uint16 bitOffsets[HID_MAX_REPORT_IDS];
	memset(bitOffsets, 0, sizeof(bitOffsets));

	size_t offset = 0;
	while (offset < size) {
		uint8 prefix = descriptor[offset++];

		// A long item carries no information this cares about.
		if (prefix == 0xfe) {
			if (offset + 1 >= size)
				break;
			uint8 dataSize = descriptor[offset];
			offset += 2 + dataSize;
			continue;
		}

		uint8 valueSize = prefix & 0x03;
		if (valueSize == 3)
			valueSize = 4;
		uint8 type = (uint8)((prefix >> 2) & 0x03);
		uint8 tag = (uint8)((prefix >> 4) & 0x0f);

		if (offset + valueSize > size)
			break;

		uint32 value = 0;
		for (uint8 i = 0; i < valueSize; i++)
			value |= (uint32)descriptor[offset + i] << (8 * i);
		offset += valueSize;

		switch (type) {
			case HID_ITEM_TYPE_GLOBAL:
				switch (tag) {
					case HID_GLOBAL_USAGE_PAGE:
						usagePage = (uint16)value;
						break;
					case HID_GLOBAL_REPORT_SIZE:
						reportSize = (uint8)value;
						break;
					case HID_GLOBAL_REPORT_COUNT:
						reportCount = (uint8)value;
						break;
					case HID_GLOBAL_REPORT_ID:
						reportId = (uint8)value;
						break;
				}
				break;

			case HID_ITEM_TYPE_LOCAL:
				switch (tag) {
					case HID_LOCAL_USAGE:
						if (namedUsageCount < HID_MAX_NAMED_USAGES)
							namedUsages[namedUsageCount++] = (uint16)value;
						break;
					case HID_LOCAL_USAGE_MINIMUM:
						usageMinimum = (uint16)value;
						break;
					case HID_LOCAL_USAGE_MAXIMUM:
						usageMaximum = (uint16)value;
						break;
				}
				break;

			case HID_ITEM_TYPE_MAIN:
			{
				if (tag == HID_MAIN_INPUT && reportSize > 0
					&& reportCount > 0) {
					uint8 slot = reportId < HID_MAX_REPORT_IDS ? reportId : 0;

					if (fItemCount < HID_MAX_ITEMS) {
						hid_item& item = fItems[fItemCount++];
						memset(&item, 0, sizeof(item));

						item.reportId = reportId;
						item.usagePage = usagePage;
						item.usageMinimum = usageMinimum;
						item.usageMaximum = usageMaximum;
						item.namedUsageCount = namedUsageCount;
						memcpy(item.namedUsages, namedUsages,
							sizeof(uint16) * namedUsageCount);

						item.bitOffset = bitOffsets[slot];
						item.bitSize = reportSize;
						item.count = reportCount;
						item.isVariable
							= (value & HID_INPUT_VARIABLE) != 0;
						item.isRelative
							= (value & HID_INPUT_RELATIVE) != 0;
						item.isConstant
							= (value & HID_INPUT_CONSTANT) != 0;
					}

					bitOffsets[slot] = (uint16)(bitOffsets[slot]
						+ (uint16)reportSize * reportCount);
				}

				// Locals apply to one main item only.
				if (tag == HID_MAIN_INPUT || tag == HID_MAIN_COLLECTION
					|| tag == HID_MAIN_END_COLLECTION
					|| (tag != HID_MAIN_INPUT && type
						== HID_ITEM_TYPE_MAIN)) {
					namedUsageCount = 0;
					usageMinimum = 0;
					usageMaximum = 0;
				}
				break;
			}
		}
	}

	return fItemCount > 0;
}


int32
HIDReportMap::_Extract(const uint8* report, size_t size, uint16 bitOffset,
	uint8 bitSize, bool isSigned) const
{
	if (bitSize == 0 || bitSize > 32)
		return 0;
	if ((size_t)(bitOffset + bitSize) > size * 8)
		return 0;

	uint32 value = 0;
	for (uint8 i = 0; i < bitSize; i++) {
		uint16 bit = (uint16)(bitOffset + i);
		if ((report[bit / 8] & (1 << (bit % 8))) != 0)
			value |= (uint32)1 << i;
	}

	if (isSigned && bitSize < 32
		&& (value & ((uint32)1 << (bitSize - 1))) != 0) {
		// Sign extend from the field's own width.
		value |= ~(((uint32)1 << bitSize) - 1);
	}

	return (int32)value;
}


bool
HIDReportMap::DecodeKeyboard(uint8 reportId, const uint8* report, size_t size,
	uint8 boot[8]) const
{
	memset(boot, 0, 8);

	bool found = false;
	uint8 keyIndex = 0;

	for (uint8 i = 0; i < fItemCount; i++) {
		const hid_item& item = fItems[i];
		if (item.reportId != reportId || item.usagePage != HID_PAGE_KEYBOARD
			|| item.isConstant) {
			continue;
		}

		if (item.isVariable) {
			// The modifier byte: eight one bit fields from Left Control up.
			for (uint8 field = 0; field < item.count; field++) {
				uint16 usage = field < item.namedUsageCount
					? item.namedUsages[field]
					: (uint16)(item.usageMinimum + field);
				if (usage < 0xe0 || usage > 0xe7)
					continue;

				if (_Extract(report, size,
						(uint16)(item.bitOffset + field * item.bitSize),
						item.bitSize, false) != 0) {
					boot[0] |= (uint8)(1 << (usage - 0xe0));
				}
				found = true;
			}
		} else {
			// An array of currently pressed usages.
			for (uint8 field = 0; field < item.count && keyIndex < 6;
					field++) {
				int32 usage = _Extract(report, size,
					(uint16)(item.bitOffset + field * item.bitSize),
					item.bitSize, false);
				if (usage <= 0x03 || usage > 0xff)
					continue;

				boot[2 + keyIndex++] = (uint8)usage;
			}
			found = true;
		}
	}

	return found;
}


bool
HIDReportMap::DecodeMouse(uint8 reportId, const uint8* report, size_t size,
	uint8 boot[4]) const
{
	memset(boot, 0, 4);

	bool found = false;

	for (uint8 i = 0; i < fItemCount; i++) {
		const hid_item& item = fItems[i];
		if (item.reportId != reportId || item.isConstant || !item.isVariable)
			continue;

		if (item.usagePage == HID_PAGE_BUTTON) {
			for (uint8 field = 0; field < item.count && field < 8; field++) {
				uint16 usage = field < item.namedUsageCount
					? item.namedUsages[field]
					: (uint16)(item.usageMinimum + field);
				if (usage < 1 || usage > 3)
					continue;

				if (_Extract(report, size,
						(uint16)(item.bitOffset + field * item.bitSize),
						item.bitSize, false) != 0) {
					boot[0] |= (uint8)(1 << (usage - 1));
				}
			}
			found = true;
			continue;
		}

		if (item.usagePage != HID_PAGE_GENERIC_DESKTOP)
			continue;

		for (uint8 field = 0; field < item.count; field++) {
			uint16 usage = field < item.namedUsageCount
				? item.namedUsages[field]
				: (uint16)(item.usageMinimum + field);

			int32 value = _Extract(report, size,
				(uint16)(item.bitOffset + field * item.bitSize), item.bitSize,
				true);

			// The boot layout is one byte per axis, so a wider field has to
			// be clamped rather than wrapped.
			if (value > 127)
				value = 127;
			else if (value < -127)
				value = -127;

			switch (usage) {
				case HID_USAGE_X:
					boot[1] = (uint8)(int8)value;
					found = true;
					break;
				case HID_USAGE_Y:
					boot[2] = (uint8)(int8)value;
					found = true;
					break;
				case HID_USAGE_WHEEL:
					boot[3] = (uint8)(int8)value;
					found = true;
					break;
			}
		}
	}

	return found;
}
