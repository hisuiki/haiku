/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "acpi_thinkpad.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ACPI.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <keyboard_mouse_driver.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <util/ring_buffer.h>
#include <usb/USB_hid.h>
#include <usb/USB_hid_page_consumer.h>
#include <usb/USB_hid_page_generic_desktop.h>


//#define TRACE_THINKPAD
#ifdef TRACE_THINKPAD
#	define TRACE(x...) dprintf("acpi_thinkpad: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("acpi_thinkpad: " x)


#define KEY_BRIGHTNESS_UP \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_DISPLAY_BRIGHTNESS_INCREMENT)
#define KEY_BRIGHTNESS_DOWN \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_DISPLAY_BRIGHTNESS_DECREMENT)
#define KEY_VOLUME_UP \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_VOLUME_INCREMENT)
#define KEY_VOLUME_DOWN \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_VOLUME_DECREMENT)
#define KEY_VOLUME_MUTE \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_MUTE)
#define KEY_MIC_MUTE \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_MUTE)
#define KEY_KBD_LIGHT \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_KEYBOARD_BRIGHTNESS_INCREMENT)
#define KEY_SLEEP \
	((B_HID_USAGE_PAGE_GENERIC_DESKTOP << 16) | B_HID_UID_GD_SYSTEM_SLEEP)
#define KEY_SEARCH \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_AC_SEARCH)
#define KEY_SETTINGS \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_AL_CONSUMER_CONTROL_CONFIGURATION)
#define KEY_CALCULATOR \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_AL_CALCULATOR)
#define KEY_FILE_BROWSER \
	((B_HID_USAGE_PAGE_CONSUMER << 16) | B_HID_UID_CON_AL_LOCAL_MACHINE_BROWSER)


static device_manager_info* sDeviceManager;


struct acpi_thinkpad_device_info {
	device_node*				node;
	acpi_device_module_info*	acpi;
	acpi_device					acpi_cookie;

	mutex						lock;
	struct ring_buffer*			key_buffer;
	sem_id						key_sem;
	int32						open_count;

	uint32						hkey_version;
	bool						has_kbdlight;
	uint8						kbdlight_level;
	bool						mute_led;
	bool						mic_mute_led;
	bool						tablet_mode;

	void						QueueKey(uint32 keycode);
	void						HandleHkeyEvent(uint32 hkey);
};


//	#pragma mark - ACPI Helpers


static status_t
evaluate_integer_method(acpi_device_module_info* acpi, acpi_device cookie,
	const char* method, uint64* outValue)
{
	acpi_data buf;
	buf.pointer = NULL;
	buf.length = ACPI_ALLOCATE_BUFFER;

	status_t status = acpi->evaluate_method(cookie, method, NULL, &buf);
	if (status != B_OK)
		return status;

	acpi_object_type* obj = (acpi_object_type*)buf.pointer;
	if (obj == NULL)
		return B_ERROR;

	if (obj->object_type == ACPI_TYPE_INTEGER) {
		if (outValue != NULL)
			*outValue = obj->integer.integer;
		free(buf.pointer);
		return B_OK;
	}

	free(buf.pointer);
	return B_BAD_TYPE;
}


static status_t
evaluate_method_int_arg(acpi_device_module_info* acpi, acpi_device cookie,
	const char* method, uint64 inArg, uint64* outValue)
{
	acpi_object_type arg;
	arg.object_type = ACPI_TYPE_INTEGER;
	arg.integer.integer = inArg;
	acpi_objects args = { 1, &arg };

	acpi_data buf;
	buf.pointer = NULL;
	buf.length = ACPI_ALLOCATE_BUFFER;

	status_t status = acpi->evaluate_method(cookie, method, &args, &buf);
	if (status != B_OK)
		return status;

	if (outValue != NULL && buf.pointer != NULL) {
		acpi_object_type* obj = (acpi_object_type*)buf.pointer;
		if (obj->object_type == ACPI_TYPE_INTEGER)
			*outValue = obj->integer.integer;
	}

	free(buf.pointer);
	return B_OK;
}


static status_t
evaluate_method_2int_args(acpi_device_module_info* acpi, acpi_device cookie,
	const char* method, uint64 arg1, uint64 arg2, uint64* outValue)
{
	acpi_object_type args[2];
	args[0].object_type = ACPI_TYPE_INTEGER;
	args[0].integer.integer = arg1;
	args[1].object_type = ACPI_TYPE_INTEGER;
	args[1].integer.integer = arg2;
	acpi_objects params = { 2, args };

	acpi_data buf;
	buf.pointer = NULL;
	buf.length = ACPI_ALLOCATE_BUFFER;

	status_t status = acpi->evaluate_method(cookie, method, &params, &buf);
	if (status != B_OK)
		return status;

	if (outValue != NULL && buf.pointer != NULL) {
		acpi_object_type* obj = (acpi_object_type*)buf.pointer;
		if (obj->object_type == ACPI_TYPE_INTEGER)
			*outValue = obj->integer.integer;
	}

	free(buf.pointer);
	return B_OK;
}


static status_t
poll_hkey_event(acpi_device_module_info* acpi, acpi_device cookie, uint32* event)
{
	acpi_data buf;
	buf.pointer = NULL;
	buf.length = ACPI_ALLOCATE_BUFFER;

	status_t status = acpi->evaluate_method(cookie, "MHKP", NULL, &buf);
	if (status != B_OK)
		return status;

	acpi_object_type* obj = (acpi_object_type*)buf.pointer;
	if (obj == NULL)
		return B_ERROR;

	if (obj->object_type == ACPI_TYPE_INTEGER) {
		*event = (uint32)obj->integer.integer;
		free(buf.pointer);
		return B_OK;
	}

	free(buf.pointer);
	return B_BAD_TYPE;
}


//	#pragma mark - Event Handling


void
acpi_thinkpad_device_info::QueueKey(uint32 keycode)
{
	MutexLocker locker(lock);
	if (open_count <= 0 || key_buffer == NULL || key_sem < 0)
		return;

	if (ring_buffer_writable(key_buffer) < (ssize_t)(sizeof(raw_key_info) * 2))
		return;

	raw_key_info keyInfo;
	keyInfo.timestamp = system_time();
	keyInfo.keycode = keycode;
	keyInfo.is_keydown = true;

	ring_buffer_write(key_buffer, (const uint8*)&keyInfo, sizeof(keyInfo));
	release_sem_etc(key_sem, 1, B_DO_NOT_RESCHEDULE);

	keyInfo.timestamp += 1000;
	keyInfo.is_keydown = false;

	ring_buffer_write(key_buffer, (const uint8*)&keyInfo, sizeof(keyInfo));
	release_sem_etc(key_sem, 1, B_DO_NOT_RESCHEDULE);
}


void
acpi_thinkpad_device_info::HandleHkeyEvent(uint32 hkey)
{
	TRACE("HKEY event: 0x%" B_PRIx32 "\n", hkey);

	switch (hkey) {
		case TP_HKEY_EV_BRIGHTNESS_UP:
			QueueKey(KEY_BRIGHTNESS_UP);
			break;

		case TP_HKEY_EV_BRIGHTNESS_DOWN:
			QueueKey(KEY_BRIGHTNESS_DOWN);
			break;

		case TP_HKEY_EV_VOL_UP:
		case 0x1319:
			QueueKey(KEY_VOLUME_UP);
			break;

		case TP_HKEY_EV_VOL_DOWN:
		case 0x1318:
			QueueKey(KEY_VOLUME_DOWN);
			break;

		case TP_HKEY_EV_VOL_MUTE:
		case 0x1317:
			mute_led = !mute_led;
			evaluate_method_int_arg(acpi, acpi_cookie, "SSMS", mute_led ? 1 : 0, NULL);
			QueueKey(KEY_VOLUME_MUTE);
			break;

		case TP_HKEY_EV_MIC_MUTE:
		case 0x1305:
		case 0x131a:
			mic_mute_led = !mic_mute_led;
			evaluate_method_int_arg(acpi, acpi_cookie, "MMTS", mic_mute_led ? 2 : 0, NULL);
			QueueKey(KEY_MIC_MUTE);
			break;

		case TP_HKEY_EV_KBD_LIGHT:
		case 0x1312:
			if (has_kbdlight) {
				kbdlight_level = (kbdlight_level + 1) % 3;
				evaluate_method_int_arg(acpi, acpi_cookie, "MLCS", kbdlight_level, NULL);
				TRACE("keyboard backlight level %u\n", kbdlight_level);
			}
			QueueKey(KEY_KBD_LIGHT);
			break;

		case TP_HKEY_EV_ZOOM:
			if (has_kbdlight) {
				kbdlight_level = (kbdlight_level + 1) % 3;
				evaluate_method_int_arg(acpi, acpi_cookie, "MLCS", kbdlight_level, NULL);
				TRACE("keyboard backlight level %u\n", kbdlight_level);
			}
			break;

		case TP_HKEY_EV_SLEEP:
			QueueKey(KEY_SLEEP);
			break;

		case TP_HKEY_EV_SETTINGS:
		case 0x1306:
			QueueKey(KEY_SETTINGS);
			break;

		case 0x1307:
			QueueKey(KEY_SEARCH);
			break;

		case 0x1308:
			QueueKey(KEY_CALCULATOR);
			break;

		case 0x1309:
			QueueKey(KEY_FILE_BROWSER);
			break;

		default:
			break;
	}
}


static void
acpi_thinkpad_notify_handler(acpi_handle handle, uint32 value, void* context)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)context;
	if (value != 0x80)
		return;

	while (true) {
		uint32 hkey = 0;
		if (poll_hkey_event(device->acpi, device->acpi_cookie, &hkey) != B_OK
			|| hkey == 0) {
			break;
		}

		device->HandleHkeyEvent(hkey);
	}
}


//	#pragma mark - Keyboard Device Module


static status_t
acpi_thinkpad_device_init(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
acpi_thinkpad_device_uninit(void* cookie)
{
}


static status_t
acpi_thinkpad_keyboard_open(void* _cookie, const char* path, int flags, void** cookie)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;
	MutexLocker locker(device->lock);

	device->open_count++;
	*cookie = device;
	return B_OK;
}


static status_t
acpi_thinkpad_keyboard_close(void* _cookie)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;
	MutexLocker locker(device->lock);

	device->open_count--;
	if (device->open_count <= 0) {
		device->open_count = 0;
		if (device->key_sem >= 0)
			release_sem_etc(device->key_sem, 1, B_DO_NOT_RESCHEDULE);
	}
	return B_OK;
}


static status_t
acpi_thinkpad_keyboard_free(void* cookie)
{
	return B_OK;
}


static status_t
acpi_thinkpad_keyboard_read(void* _cookie, off_t position, void* buffer, size_t* num_bytes)
{
	*num_bytes = 0;
	return B_ERROR;
}


static status_t
acpi_thinkpad_keyboard_write(void* _cookie, off_t position, const void* buffer, size_t* num_bytes)
{
	*num_bytes = 0;
	return B_ERROR;
}


static status_t
acpi_thinkpad_keyboard_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;

	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			const char name[] = "ThinkPad Extra Keys";
			if (user_strlcpy((char*)buffer, name, length) < 0)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case KB_READ:
		{
			if (buffer == NULL || length < sizeof(raw_key_info))
				return B_BAD_VALUE;

			while (true) {
				status_t status = acquire_sem_etc(device->key_sem, 1,
					B_CAN_INTERRUPT, 0);
				if (status != B_OK)
					return status;

				MutexLocker locker(device->lock);
				if (device->open_count <= 0)
					return B_INTERRUPTED;

				raw_key_info keyInfo;
				if (ring_buffer_read(device->key_buffer, (uint8*)&keyInfo,
						sizeof(raw_key_info)) == sizeof(raw_key_info)) {
					if (user_memcpy(buffer, &keyInfo, sizeof(raw_key_info)) != B_OK)
						return B_BAD_ADDRESS;
					return B_OK;
				}
			}
		}

		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_GET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
		case KB_GET_KEY_REPEAT_DELAY:
			return B_OK;

		default:
			return B_DEV_INVALID_IOCTL;
	}
}


//	#pragma mark - Power / Control Device Module


static status_t
acpi_thinkpad_device_open(void* _cookie, const char* path, int flags, void** cookie)
{
	*cookie = _cookie;
	return B_OK;
}


static status_t
acpi_thinkpad_device_close(void* cookie)
{
	return B_OK;
}


static status_t
acpi_thinkpad_device_free(void* cookie)
{
	return B_OK;
}


static status_t
acpi_thinkpad_device_read(void* _cookie, off_t position, void* buffer, size_t* num_bytes)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;
	if (position < 0)
		return B_BAD_VALUE;

	char status[256];
	snprintf(status, sizeof(status),
		"ThinkPad ACPI HKEY v0x%" B_PRIx32 "\n"
		"Keyboard Backlight: %s (level %u)\n"
		"Audio Mute LED: %s\n"
		"Mic Mute LED: %s\n"
		"Tablet Mode: %s\n",
		device->hkey_version,
		device->has_kbdlight ? "supported" : "unsupported",
		device->kbdlight_level,
		device->mute_led ? "on" : "off",
		device->mic_mute_led ? "on" : "off",
		device->tablet_mode ? "active" : "inactive");

	size_t len = strlen(status);
	if ((size_t)position >= len) {
		*num_bytes = 0;
		return B_OK;
	}

	size_t toCopy = min_c(*num_bytes, len - (size_t)position);
	if (user_memcpy(buffer, status + position, toCopy) != B_OK)
		return B_BAD_ADDRESS;

	*num_bytes = toCopy;
	return B_OK;
}


static status_t
acpi_thinkpad_device_write(void* cookie, off_t position, const void* buffer, size_t* num_bytes)
{
	*num_bytes = 0;
	return B_ERROR;
}


static status_t
acpi_thinkpad_device_control(void* _cookie, uint32 op, void* arg, size_t len)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;

	switch (op) {
		case THINKPAD_GET_BACKLIGHT_LEVEL:
		{
			if (!device->has_kbdlight)
				return B_NOT_SUPPORTED;
			if (arg == NULL || len < sizeof(uint8))
				return B_BAD_VALUE;

			uint64 status = 0;
			if (evaluate_method_int_arg(device->acpi, device->acpi_cookie, "MLCG", 0, &status) == B_OK) {
				device->kbdlight_level = (uint8)(status & 0x3);
			}

			if (user_memcpy(arg, &device->kbdlight_level, sizeof(uint8)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case THINKPAD_SET_BACKLIGHT_LEVEL:
		{
			if (!device->has_kbdlight)
				return B_NOT_SUPPORTED;
			if (arg == NULL || len < sizeof(uint8))
				return B_BAD_VALUE;

			uint8 level;
			if (user_memcpy(&level, arg, sizeof(uint8)) != B_OK)
				return B_BAD_ADDRESS;

			if (level > 2)
				return B_BAD_VALUE;

			status_t status = evaluate_method_int_arg(device->acpi, device->acpi_cookie,
				"MLCS", level, NULL);
			if (status == B_OK)
				device->kbdlight_level = level;
			return status;
		}

		case THINKPAD_GET_HKEY_VERSION:
		{
			if (arg == NULL || len < sizeof(uint32))
				return B_BAD_VALUE;
			if (user_memcpy(arg, &device->hkey_version, sizeof(uint32)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case THINKPAD_GET_TABLET_MODE:
		{
			if (arg == NULL || len < sizeof(bool))
				return B_BAD_VALUE;

			uint64 status = 0;
			if (evaluate_integer_method(device->acpi, device->acpi_cookie, "MHKG", &status) == B_OK)
				device->tablet_mode = (status != 0);

			if (user_memcpy(arg, &device->tablet_mode, sizeof(bool)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case THINKPAD_SET_MUTE_LED:
		{
			if (arg == NULL || len < sizeof(bool))
				return B_BAD_VALUE;
			bool on;
			if (user_memcpy(&on, arg, sizeof(bool)) != B_OK)
				return B_BAD_ADDRESS;
			device->mute_led = on;
			return evaluate_method_int_arg(device->acpi, device->acpi_cookie, "SSMS",
				on ? 1 : 0, NULL);
		}

		case THINKPAD_SET_MIC_MUTE_LED:
		{
			if (arg == NULL || len < sizeof(bool))
				return B_BAD_VALUE;
			bool on;
			if (user_memcpy(&on, arg, sizeof(bool)) != B_OK)
				return B_BAD_ADDRESS;
			device->mic_mute_led = on;
			return evaluate_method_int_arg(device->acpi, device->acpi_cookie, "MMTS",
				on ? 2 : 0, NULL);
		}

		default:
			return B_DEV_INVALID_IOCTL;
	}
}


//	#pragma mark - Driver Module API


static float
acpi_thinkpad_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false))
		return -1;

	if (strcmp(bus, "acpi") != 0)
		return 0.0;

	uint32 device_type;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&device_type, false) != B_OK
		|| device_type != ACPI_TYPE_DEVICE) {
		return 0.0;
	}

	const char* hid = NULL;
	sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid, false);

	const char* cid = NULL;
	sDeviceManager->get_attr_string(parent, ACPI_DEVICE_CID_ITEM, &cid, false);

	bool match = false;
	if (hid != NULL) {
		if (strcasecmp(hid, "IBM0068") == 0
			|| strcasecmp(hid, "LEN0068") == 0
			|| strcasecmp(hid, "LEN0268") == 0) {
			match = true;
		}
	}
	if (!match && cid != NULL) {
		if (strstr(cid, "IBM0068") != NULL
			|| strstr(cid, "LEN0068") != NULL
			|| strstr(cid, "LEN0268") != NULL) {
			match = true;
		}
	}

	if (!match)
		return 0.0;

	TRACE("Found ThinkPad HKEY device: %s\n", hid != NULL ? hid : cid);
	return 0.8f;
}


static status_t
acpi_thinkpad_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "ThinkPad ACPI Extras" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, ACPI_THINKPAD_DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
acpi_thinkpad_init_driver(device_node* node, void** driverCookie)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)calloc(1,
		sizeof(acpi_thinkpad_device_info));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->acpi,
		(void**)&device->acpi_cookie);
	sDeviceManager->put_node(parent);

	mutex_init(&device->lock, "acpi thinkpad lock");

	device->key_buffer = create_ring_buffer(sizeof(raw_key_info) * 64);
	if (device->key_buffer == NULL) {
		mutex_destroy(&device->lock);
		free(device);
		return B_NO_MEMORY;
	}

	device->key_sem = create_sem(0, "thinkpad_key_sem");
	if (device->key_sem < 0) {
		delete_ring_buffer(device->key_buffer);
		mutex_destroy(&device->lock);
		free(device);
		return B_NO_MEMORY;
	}

	device->open_count = 0;
	device->mute_led = false;
	device->mic_mute_led = false;
	device->has_kbdlight = false;
	device->kbdlight_level = 0;
	device->tablet_mode = false;

	// Enable HKEY event interface
	evaluate_method_int_arg(device->acpi, device->acpi_cookie, "MHKC", 1, NULL);

	// Query HKEY interface version
	uint64 version = 0;
	if (evaluate_integer_method(device->acpi, device->acpi_cookie, "MHKV", &version) == B_OK)
		device->hkey_version = (uint32)version;
	else
		device->hkey_version = 0x0100;

	TRACE("HKEY interface version: 0x%" B_PRIx32 "\n", device->hkey_version);

	// Unmask all 32 hotkey events
	for (uint32 i = 1; i <= 32; i++) {
		evaluate_method_2int_args(device->acpi, device->acpi_cookie, "MHKM", i, 1, NULL);
	}

	// Check keyboard backlight support
	uint64 kbdStatus = 0;
	if (evaluate_method_int_arg(device->acpi, device->acpi_cookie, "MLCG", 0, &kbdStatus) == B_OK) {
		if ((kbdStatus & (1 << 9)) != 0) {
			device->has_kbdlight = true;
			device->kbdlight_level = (uint8)(kbdStatus & 0x3);
			TRACE("Keyboard backlight supported, level: %u\n", device->kbdlight_level);
		}
	}

	// Check tablet mode
	uint64 tabletStatus = 0;
	if (evaluate_integer_method(device->acpi, device->acpi_cookie, "MHKG", &tabletStatus) == B_OK) {
		device->tablet_mode = (tabletStatus != 0);
	}

	// Install ACPI notify handler
	status_t status = device->acpi->install_notify_handler(device->acpi_cookie,
		ACPI_DEVICE_NOTIFY, acpi_thinkpad_notify_handler, device);
	if (status != B_OK) {
		ERROR("Failed to install ACPI notify handler: %s\n", strerror(status));
	}

	*driverCookie = device;
	return B_OK;
}


static void
acpi_thinkpad_uninit_driver(void* driverCookie)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)driverCookie;

	device->acpi->remove_notify_handler(device->acpi_cookie,
		ACPI_DEVICE_NOTIFY, acpi_thinkpad_notify_handler);

	if (device->key_sem >= 0)
		delete_sem(device->key_sem);

	if (device->key_buffer != NULL)
		delete_ring_buffer(device->key_buffer);

	mutex_destroy(&device->lock);
	free(device);
}


static status_t
acpi_thinkpad_register_child_devices(void* _cookie)
{
	acpi_thinkpad_device_info* device = (acpi_thinkpad_device_info*)_cookie;
	status_t status;

	int32 pathId = sDeviceManager->create_id("acpi_thinkpad/path_id");
	if (pathId < 0)
		pathId = 0;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), "input/keyboard/thinkpad/%" B_PRId32, pathId);
	status = sDeviceManager->publish_device(device->node, name,
		ACPI_THINKPAD_KEYBOARD_MODULE_NAME);
	if (status != B_OK)
		ERROR("failed to publish %s: %s\n", name, strerror(status));

	snprintf(name, sizeof(name), "power/thinkpad/%" B_PRId32, pathId);
	status = sDeviceManager->publish_device(device->node, name,
		ACPI_THINKPAD_DEVICE_MODULE_NAME);
	if (status != B_OK)
		ERROR("failed to publish %s: %s\n", name, strerror(status));

	return B_OK;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


driver_module_info acpi_thinkpad_driver_module = {
	{
		ACPI_THINKPAD_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	acpi_thinkpad_support,
	acpi_thinkpad_register_device,
	acpi_thinkpad_init_driver,
	acpi_thinkpad_uninit_driver,
	acpi_thinkpad_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info acpi_thinkpad_keyboard_module = {
	{
		ACPI_THINKPAD_KEYBOARD_MODULE_NAME,
		0,
		NULL
	},

	acpi_thinkpad_device_init,
	acpi_thinkpad_device_uninit,
	NULL,

	acpi_thinkpad_keyboard_open,
	acpi_thinkpad_keyboard_close,
	acpi_thinkpad_keyboard_free,
	acpi_thinkpad_keyboard_read,
	acpi_thinkpad_keyboard_write,
	NULL,
	acpi_thinkpad_keyboard_control,
	NULL,
	NULL
};


struct device_module_info acpi_thinkpad_device_module = {
	{
		ACPI_THINKPAD_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	acpi_thinkpad_device_init,
	acpi_thinkpad_device_uninit,
	NULL,

	acpi_thinkpad_device_open,
	acpi_thinkpad_device_close,
	acpi_thinkpad_device_free,
	acpi_thinkpad_device_read,
	acpi_thinkpad_device_write,
	NULL,
	acpi_thinkpad_device_control,
	NULL,
	NULL
};


module_info* modules[] = {
	(module_info*)&acpi_thinkpad_driver_module,
	(module_info*)&acpi_thinkpad_keyboard_module,
	(module_info*)&acpi_thinkpad_device_module,
	NULL
};
