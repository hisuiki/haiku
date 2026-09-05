/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_HID_REPORT_MAP_H_
#define _BLUETOOTH_HID_REPORT_MAP_H_

#include <SupportDefs.h>


#define HID_MAX_ITEMS			32
#define HID_MAX_NAMED_USAGES	16

// Usage pages that matter to a keyboard or a mouse.
#define HID_PAGE_GENERIC_DESKTOP	0x01
#define HID_PAGE_KEYBOARD			0x07
#define HID_PAGE_BUTTON				0x09

#define HID_USAGE_X					0x30
#define HID_USAGE_Y					0x31
#define HID_USAGE_WHEEL				0x38


/*!	One Input item of a report descriptor: a run of fields sharing a size and
	a usage page, at a known bit offset within its report.
*/
struct hid_item {
	uint8	reportId;
	uint16	usagePage;
	uint16	usageMinimum;
	uint16	usageMaximum;
	uint16	namedUsages[HID_MAX_NAMED_USAGES];
	uint8	namedUsageCount;

	uint16	bitOffset;
	uint8	bitSize;
	uint8	count;

	bool	isVariable;
	bool	isRelative;
	bool	isConstant;
};


/*!	What a report descriptor says about a peripheral's input reports, reduced
	to what is needed to turn one into key and pointer events.
*/
class HIDReportMap {
public:
							HIDReportMap();

			bool			Parse(const uint8* descriptor, size_t size);
			bool			IsValid() const { return fItemCount > 0; }

	/*!	Rewrites one report into the eight byte boot keyboard layout, which
		is what the rest of the add-on already understands. Returns false if
		this report carries no keyboard input.
	*/
			bool			DecodeKeyboard(uint8 reportId, const uint8* report,
								size_t size, uint8 boot[8]) const;

	/*!	Rewrites one report into the boot mouse layout: buttons, then relative
		x, y and wheel, each clamped to a byte.
	*/
			bool			DecodeMouse(uint8 reportId, const uint8* report,
								size_t size, uint8 boot[4]) const;

private:
			int32			_Extract(const uint8* report, size_t size,
								uint16 bitOffset, uint8 bitSize,
								bool isSigned) const;

			hid_item		fItems[HID_MAX_ITEMS];
			uint8			fItemCount;
};

#endif // _BLUETOOTH_HID_REPORT_MAP_H_
