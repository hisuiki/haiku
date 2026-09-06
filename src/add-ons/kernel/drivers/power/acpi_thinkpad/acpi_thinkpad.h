/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ACPI_THINKPAD_H
#define _ACPI_THINKPAD_H

#include <Drivers.h>

#define ACPI_THINKPAD_DRIVER_MODULE_NAME	"drivers/power/acpi_thinkpad/driver_v1"
#define ACPI_THINKPAD_DEVICE_MODULE_NAME	"drivers/power/acpi_thinkpad/device_v1"
#define ACPI_THINKPAD_KEYBOARD_MODULE_NAME	"drivers/power/acpi_thinkpad/keyboard_v1"

/* ThinkPad ACPI HKEY events */
enum {
	TP_HKEY_EV_FN_F1			= 0x1001,
	TP_HKEY_EV_FN_F2			= 0x1002, // Screen lock / Coffee
	TP_HKEY_EV_FN_F3			= 0x1003, // Battery
	TP_HKEY_EV_SLEEP			= 0x1004, // Fn+F4: Sleep
	TP_HKEY_EV_WIFI				= 0x1005, // Fn+F5: Radio / WLAN
	TP_HKEY_EV_CAMERA			= 0x1006, // Fn+F6: Camera
	TP_HKEY_EV_DISPLAY			= 0x1007, // Fn+F7: Video / Display output
	TP_HKEY_EV_SETTINGS			= 0x1008, // Fn+F8: Settings
	TP_HKEY_EV_FN_F9			= 0x1009,
	TP_HKEY_EV_FN_F10			= 0x100a,
	TP_HKEY_EV_FN_F11			= 0x100b,
	TP_HKEY_EV_HIBERNATE		= 0x100c, // Fn+F12: Hibernate
	TP_HKEY_EV_BRIGHTNESS_UP	= 0x1010, // Fn+Home: Brightness up
	TP_HKEY_EV_BRIGHTNESS_DOWN	= 0x1011, // Fn+End: Brightness down
	TP_HKEY_EV_KBD_LIGHT		= 0x1012, // Fn+PageUp / Fn+Space: Keyboard backlight
	TP_HKEY_EV_ZOOM				= 0x1014, // Fn+Space on older models
	TP_HKEY_EV_VOL_UP			= 0x1015, // Volume up
	TP_HKEY_EV_VOL_DOWN			= 0x1016, // Volume down
	TP_HKEY_EV_VOL_MUTE			= 0x1017, // Volume mute
	TP_HKEY_EV_MIC_MUTE			= 0x101b, // Microphone mute
};

/* ioctl opcodes for /dev/power/thinkpad */
enum {
	THINKPAD_GET_BACKLIGHT_LEVEL = B_DEVICE_OP_CODES_END + 1,
	THINKPAD_SET_BACKLIGHT_LEVEL,
	THINKPAD_GET_HKEY_VERSION,
	THINKPAD_GET_TABLET_MODE,
	THINKPAD_SET_MUTE_LED,
	THINKPAD_SET_MIC_MUTE_LED,
};

#endif /* _ACPI_THINKPAD_H */
