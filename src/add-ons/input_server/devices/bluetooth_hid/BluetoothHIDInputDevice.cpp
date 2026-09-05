/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <add-ons/input_server/InputServerDevice.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/l2cap.h>
#include <BluetoothHID.h>

#include "HIDReportMap.h"

#include <Autolock.h>
#include <AutoDeleter.h>
#include <Keymap.h>

#include <kb_mouse_settings.h>
#include <Locker.h>
#include <Message.h>
#include <View.h>
#include <OS.h>

#include <errno.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


// HIDP transaction headers, see the Bluetooth HID profile specification.
static const uint8 kHIDPHandshakeSuccessful = 0x00;
static const uint8 kHIDPSetBootProtocol = 0x70;
static const uint8 kHIDPDataInput = 0xa1;

// A boot keyboard report is one modifier byte, one reserved byte and six
// concurrently pressed usages.
static const size_t kBootKeyboardReportSize = 8;

static const int32 kMaxConnections = 8;

// A device that is out of range must not block its connection thread forever.
// L2capEndpoint::Connect() bounds its wait by SO_SNDTIMEO.
static const bigtime_t kConnectTimeout = 20000000;

// The control channel handshake is answered immediately by a working device.
static const bigtime_t kHandshakeTimeout = 5000000;

// An idle interrupt channel is polled at this interval so that connection
// threads notice in reasonable time that the add-on is going away.
static const bigtime_t kIdleReceiveTimeout = 1000000;


class BluetoothHIDInputDevice;


struct hid_connection {
	BluetoothHIDInputDevice* owner;
	bdaddr_t	address;
	// A Low Energy peripheral has no sockets and no thread of its own: its
	// reports arrive on the port, delivered by the kernel's HID over GATT
	// support, so only the state below is used for one.
	bool		lowEnergy;
	int			control;
	int			interrupt;
	thread_id	thread;

	// Key state of this device, mirroring what KeyboardInputDevice keeps for
	// a single physical keyboard.
	uint8		states[16];
	uint32		modifiers;
	uint8		lastReport[kBootKeyboardReportSize];
	uint32		lastKeyCode;
	int32		repeatCount;

	// The host is responsible for auto repeat, boot keyboards only report
	// state changes.
	uint8		repeatUsage;
	bigtime_t	repeatDeadline;

	uint8		lastButtons;
	// Acceleration works in floating point, and the remainder of each report
	// is carried into the next one so slow movement is not rounded away.
	float		historyDeltaX;
	float		historyDeltaY;
	// Click counting, which is what turns two presses into a double click.
	bigtime_t	lastClickTime;
	uint32		lastClickButtons;
	int32		clickCount;

	// Only for a peripheral in report protocol mode, whose report layout has
	// to be read out of its own descriptor.
	HIDReportMap* reportMap;
};


class BluetoothHIDInputDevice : public BInputServerDevice {
public:
								BluetoothHIDInputDevice();
	virtual						~BluetoothHIDInputDevice();

	virtual	status_t			InitCheck();
	virtual	status_t			Start(const char* name, void* cookie);
	virtual	status_t			Stop(const char* name, void* cookie);
	virtual	status_t			Control(const char* name, void* cookie,
									uint32 command, BMessage* message);

private:
	static	status_t			_ListenerEntry(void* cookie);
			status_t			_Listener();
			void				_ProcessRepeats();

			hid_connection*		_LowEnergyConnection(const bdaddr_t& address);
			void				_LowEnergyReport(
									const bluetooth_hid_report* report);
			void				_LowEnergyDisconnected(
									const bdaddr_t& address);
			void				_LowEnergyReportMap(
									const bluetooth_hid_report_map* map);

			status_t			_Connect(const bdaddr_t& address);
	static	status_t			_ConnectionEntry(void* cookie);
			void				_Connection(hid_connection* connection);
			void				_Disconnect(hid_connection* connection);

			void				_ReadReports(hid_connection* connection);
			void				_KeyboardReport(hid_connection* connection,
									const uint8* report);
			void				_MouseReport(hid_connection* connection,
									const uint8* report, size_t size);
			void				_Key(hid_connection* connection, uint8 usage,
									bool down);
			void				_UpdateSettings(uint32 command);
			void				_UpdateMouseSettings();
			uint32				_RemapButtons(uint32 buttons) const;
			void				_ComputeAcceleration(hid_connection* connection,
									int32 x, int32 y, int32& deltaX,
									int32& deltaY) const;

	static	uint32				_KeyCode(uint8 usage);
	static	bool				_ContainsUsage(const uint8* report,
									uint8 usage);

private:
			port_id				fPort;
			thread_id			fListener;
			bool				fRunning;
			int32				fStartedDevices;

			BLocker				fConnectionLock;
			hid_connection*		fConnections[kMaxConnections];

			BLocker				fKeymapLock;
			BKeymap				fKeymap;
			bigtime_t			fRepeatDelay;
			bigtime_t			fRepeatRate;

			mouse_settings		fMouseSettings;

			input_device_ref	fKeyboard;
			input_device_ref	fMouse;
			input_device_ref*	fDevices[3];
};


static void
set_socket_timeout(int socketFD, int option, bigtime_t timeout)
{
	timeval value;
	value.tv_sec = timeout / 1000000;
	value.tv_usec = timeout % 1000000;
	setsockopt(socketFD, SOL_SOCKET, option, &value, sizeof(value));
}


static int
connect_l2cap(const bdaddr_t& address, uint16 psm)
{
	int socketFD = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (socketFD < 0)
		return -1;

	set_socket_timeout(socketFD, SO_SNDTIMEO, kConnectTimeout);

	sockaddr_l2cap peer;
	memset(&peer, 0, sizeof(peer));
	peer.l2cap_len = sizeof(peer);
	peer.l2cap_family = AF_BLUETOOTH;
	peer.l2cap_bdaddr = address;
	peer.l2cap_psm = psm;
	if (connect(socketFD, (sockaddr*)&peer, sizeof(peer)) < 0) {
		close(socketFD);
		return -1;
	}

	return socketFD;
}


BluetoothHIDInputDevice::BluetoothHIDInputDevice()
	:
	fPort(-1),
	fListener(-1),
	fRunning(false),
	fStartedDevices(0),
	fConnectionLock("bluetooth HID connections"),
	fKeymapLock("bluetooth HID keymap"),
	fRepeatDelay(250000),
	fRepeatRate(50000)
{
	memset(fConnections, 0, sizeof(fConnections));

	fKeyboard.name = strdup("Bluetooth HID Keyboard");
	fKeyboard.type = B_KEYBOARD_DEVICE;
	fKeyboard.cookie = &fKeyboard;
	fMouse.name = strdup("Bluetooth HID Mouse");
	fMouse.type = B_POINTING_DEVICE;
	fMouse.cookie = &fMouse;
	fDevices[0] = &fKeyboard;
	fDevices[1] = &fMouse;
	fDevices[2] = NULL;
}


BluetoothHIDInputDevice::~BluetoothHIDInputDevice()
{
	fRunning = false;

	// Stop the listener first, so that it cannot add a connection behind the
	// back of the teardown below.
	if (fPort >= B_OK)
		delete_port(fPort);
	if (fListener >= B_OK) {
		status_t result;
		wait_for_thread(fListener, &result);
	}

	// Shutting the channels down makes the connection threads return from
	// their blocking recv().
	thread_id threads[kMaxConnections];
	int32 count = 0;
	if (fConnectionLock.Lock()) {
		for (int32 i = 0; i < kMaxConnections; i++) {
			hid_connection* connection = fConnections[i];
			if (connection == NULL)
				continue;

			if (connection->lowEnergy) {
				// No sockets and no thread, so it can simply go.
				fConnections[i] = NULL;
				delete connection->reportMap;
				delete connection;
				continue;
			}

			threads[count++] = connection->thread;
			shutdown(connection->interrupt, SHUT_RDWR);
			shutdown(connection->control, SHUT_RDWR);
		}
		fConnectionLock.Unlock();
	}

	for (int32 i = 0; i < count; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
	}

	free(fKeyboard.name);
	free(fMouse.name);
}


status_t
BluetoothHIDInputDevice::InitCheck()
{
	if (fKeyboard.name == NULL || fMouse.name == NULL)
		return B_NO_MEMORY;

	status_t status = fKeymapLock.InitCheck();
	if (status != B_OK)
		return status;
	status = fConnectionLock.InitCheck();
	if (status != B_OK)
		return status;

	_UpdateSettings(0);
	_UpdateMouseSettings();

	status = fKeymap.SetToCurrent();
	if (status != B_OK)
		return status;

	// Deep enough to absorb a burst of movement reports: a mouse in hand
	// produces them far faster than a connection request arrives.
	fPort = create_port(128, BLUETOOTH_HID_PORT_NAME);
	if (fPort < B_OK)
		return fPort;

	status = RegisterDevices(fDevices);
	if (status != B_OK)
		return status;

	fRunning = true;
	fListener = spawn_thread(_ListenerEntry, "bluetooth HID listener",
		B_NORMAL_PRIORITY, this);
	if (fListener < B_OK) {
		fRunning = false;
		return fListener;
	}

	return resume_thread(fListener);
}


status_t
BluetoothHIDInputDevice::Start(const char*, void*)
{
	atomic_add(&fStartedDevices, 1);
	return B_OK;
}


status_t
BluetoothHIDInputDevice::Stop(const char*, void*)
{
	atomic_add(&fStartedDevices, -1);
	return B_OK;
}


status_t
BluetoothHIDInputDevice::Control(const char*, void*, uint32 command,
	BMessage*)
{
	if (command >= B_KEY_MAP_CHANGED && command <= B_KEY_REPEAT_RATE_CHANGED)
		_UpdateSettings(command);

	if (command >= B_MOUSE_TYPE_CHANGED
		&& command <= B_MOUSE_ACCELERATION_CHANGED) {
		_UpdateMouseSettings();
	}

	return B_OK;
}


void
BluetoothHIDInputDevice::_UpdateMouseSettings()
{
	BAutolock lock(fKeymapLock);

	if (get_click_speed(fMouse.name, &fMouseSettings.click_speed) != B_OK)
		fMouseSettings.click_speed = kDefaultClickSpeed;
	if (get_mouse_speed(fMouse.name, &fMouseSettings.accel.speed) != B_OK)
		fMouseSettings.accel.speed = kDefaultMouseSpeed;
	if (get_mouse_acceleration(fMouse.name,
			&fMouseSettings.accel.accel_factor) != B_OK) {
		fMouseSettings.accel.accel_factor = kDefaultAccelerationFactor;
	}
	if (get_mouse_map(fMouse.name, &fMouseSettings.map) != B_OK) {
		fMouseSettings.map.button[0] = B_PRIMARY_MOUSE_BUTTON;
		fMouseSettings.map.button[1] = B_SECONDARY_MOUSE_BUTTON;
		fMouseSettings.map.button[2] = B_TERTIARY_MOUSE_BUTTON;
	}
}


uint32
BluetoothHIDInputDevice::_RemapButtons(uint32 buttons) const
{
	uint32 remapped = 0;
	for (int32 i = 0; buttons != 0; i++) {
		if ((buttons & 1) != 0)
			remapped |= fMouseSettings.map.button[i];
		buttons >>= 1;
	}

	return remapped;
}


/*!	The same curve the ordinary mouse device applies, so a Bluetooth pointer
	moves at the speed the Mouse preferences ask for.
*/
void
BluetoothHIDInputDevice::_ComputeAcceleration(hid_connection* connection,
	int32 x, int32 y, int32& deltaX, int32& deltaY) const
{
	float floatX = (float)x * fMouseSettings.accel.speed / 65536.0f
		+ connection->historyDeltaX;
	float floatY = (float)y * fMouseSettings.accel.speed / 65536.0f
		+ connection->historyDeltaY;

	double acceleration = 1;
	if (fMouseSettings.accel.accel_factor != 0) {
		acceleration = 1 + sqrt(floatX * floatX + floatY * floatY)
			* fMouseSettings.accel.accel_factor / 524288.0;
	}

	floatX *= acceleration;
	floatY *= acceleration;

	deltaX = floatX >= 0 ? (int32)floorf(floatX) : (int32)ceilf(floatX);
	deltaY = floatY >= 0 ? (int32)floorf(floatY) : (int32)ceilf(floatY);

	// Carry the fraction over, so slow movement is not lost to rounding.
	connection->historyDeltaX = floatX - deltaX;
	connection->historyDeltaY = floatY - deltaY;
}


void
BluetoothHIDInputDevice::_UpdateSettings(uint32 command)
{
	BAutolock lock(fKeymapLock);

	if (command == 0 || command == B_KEY_REPEAT_DELAY_CHANGED) {
		bigtime_t delay;
		if (get_key_repeat_delay(&delay) == B_OK && delay > 0)
			fRepeatDelay = delay;
	}

	if (command == 0 || command == B_KEY_REPEAT_RATE_CHANGED) {
		int32 rate;
		if (get_key_repeat_rate(&rate) == B_OK && rate > 0 && rate <= 1000000)
			fRepeatRate = 10000000LL / rate;
	}

	if (command == B_KEY_MAP_CHANGED)
		fKeymap.SetToCurrent();
}


status_t
BluetoothHIDInputDevice::_ListenerEntry(void* cookie)
{
	return ((BluetoothHIDInputDevice*)cookie)->_Listener();
}


status_t
BluetoothHIDInputDevice::_Listener()
{
	while (fRunning) {
		// A Low Energy peripheral has no thread of its own to run auto repeat
		// from, so this loop wakes up to do it for them.
		bigtime_t timeout = kIdleReceiveTimeout;
		if (fConnectionLock.Lock()) {
			for (int32 i = 0; i < kMaxConnections; i++) {
				hid_connection* connection = fConnections[i];
				if (connection == NULL || !connection->lowEnergy
					|| connection->repeatUsage == 0) {
					continue;
				}

				bigtime_t remaining
					= connection->repeatDeadline - system_time();
				if (remaining < 1000)
					remaining = 1000;
				if (remaining < timeout)
					timeout = remaining;
			}
			fConnectionLock.Unlock();
		}

		char message[sizeof(bluetooth_hid_report_map)];
		int32 code;
		ssize_t size = read_port_etc(fPort, &code, message, sizeof(message),
			B_RELATIVE_TIMEOUT, timeout);
		if (size < B_OK) {
			if (size == B_TIMED_OUT || size == B_INTERRUPTED
				|| size == B_WOULD_BLOCK) {
				_ProcessRepeats();
				continue;
			}

			// Anything else means the port is gone.
			break;
		}

		switch (code) {
			case BLUETOOTH_HID_CONNECT:
				if ((size_t)size == sizeof(bluetooth_hid_connect_request)) {
					_Connect(((bluetooth_hid_connect_request*)message)
						->address);
				}
				break;

			case BLUETOOTH_HID_REPORT:
				if ((size_t)size == sizeof(bluetooth_hid_report))
					_LowEnergyReport((bluetooth_hid_report*)message);
				break;

			case BLUETOOTH_HID_REPORT_MAP:
				if ((size_t)size == sizeof(bluetooth_hid_report_map))
					_LowEnergyReportMap((bluetooth_hid_report_map*)message);
				break;

			case BLUETOOTH_HID_DISCONNECTED:
				if ((size_t)size == sizeof(bluetooth_hid_disconnected)) {
					_LowEnergyDisconnected(
						((bluetooth_hid_disconnected*)message)->address);
				}
				break;

			default:
				break;
		}

		_ProcessRepeats();
	}

	return B_OK;
}


void
BluetoothHIDInputDevice::_ProcessRepeats()
{
	BAutolock lock(fConnectionLock);

	for (int32 i = 0; i < kMaxConnections; i++) {
		hid_connection* connection = fConnections[i];
		if (connection == NULL || !connection->lowEnergy
			|| connection->repeatUsage == 0) {
			continue;
		}

		if (system_time() < connection->repeatDeadline)
			continue;

		uint8 usage = connection->repeatUsage;
		_Key(connection, usage, true);
		// _Key() restarts the initial delay; the repeats that follow come at
		// the configured rate instead.
		connection->repeatUsage = usage;
		connection->repeatDeadline = system_time() + fRepeatRate;
	}
}


/*!	Finds the state kept for one Low Energy peripheral, creating it the first
	time a report arrives from that address.
*/
hid_connection*
BluetoothHIDInputDevice::_LowEnergyConnection(const bdaddr_t& address)
{
	int32 slot = -1;
	for (int32 i = 0; i < kMaxConnections; i++) {
		if (fConnections[i] == NULL) {
			if (slot < 0)
				slot = i;
		} else if (Bluetooth::bdaddrUtils::Compare(fConnections[i]->address,
				address)) {
			return fConnections[i];
		}
	}

	if (slot < 0)
		return NULL;

	hid_connection* connection = new(std::nothrow) hid_connection;
	if (connection == NULL)
		return NULL;

	memset(connection, 0, sizeof(hid_connection));
	connection->owner = this;
	connection->address = address;
	connection->lowEnergy = true;
	connection->control = -1;
	connection->interrupt = -1;
	connection->thread = -1;
	connection->repeatDeadline = B_INFINITE_TIMEOUT;

	fConnections[slot] = connection;

	fprintf(stderr, "bluetooth_hid: %s connected over Low Energy\n",
		Bluetooth::bdaddrUtils::ToString(address).String());

	return connection;
}


void
BluetoothHIDInputDevice::_LowEnergyReport(const bluetooth_hid_report* report)
{
	if (fStartedDevices == 0)
		return;

	BAutolock lock(fConnectionLock);

	hid_connection* connection = _LowEnergyConnection(report->address);
	if (connection == NULL)
		return;

	if (report->type == BLUETOOTH_HID_KEYBOARD_REPORT
		&& report->size >= kBootKeyboardReportSize) {
		_KeyboardReport(connection, report->data);
	} else if (report->type == BLUETOOTH_HID_MOUSE_REPORT
		&& report->size >= 3) {
		_MouseReport(connection, report->data, report->size);
	} else if (report->type == BLUETOOTH_HID_GENERIC_REPORT
		&& connection->reportMap != NULL) {
		// Rewrite it into the boot layout, so everything below this point
		// stays the same whichever protocol mode the device is in.
		uint8 keyboard[kBootKeyboardReportSize];
		if (connection->reportMap->DecodeKeyboard(report->reportId,
				report->data, report->size, keyboard)) {
			_KeyboardReport(connection, keyboard);
		}

		uint8 mouse[4];
		if (connection->reportMap->DecodeMouse(report->reportId, report->data,
				report->size, mouse)) {
			_MouseReport(connection, mouse, sizeof(mouse));
		}
	}
}


void
BluetoothHIDInputDevice::_LowEnergyReportMap(
	const bluetooth_hid_report_map* map)
{
	if (map->size == 0 || map->size > BLUETOOTH_HID_MAX_REPORT_MAP)
		return;

	BAutolock lock(fConnectionLock);

	hid_connection* connection = _LowEnergyConnection(map->address);
	if (connection == NULL)
		return;

	if (connection->reportMap == NULL) {
		connection->reportMap = new(std::nothrow) HIDReportMap();
		if (connection->reportMap == NULL)
			return;
	}

	if (connection->reportMap->Parse(map->data, map->size)) {
		fprintf(stderr, "bluetooth_hid: %s described %d bytes of reports\n",
			Bluetooth::bdaddrUtils::ToString(map->address).String(),
			map->size);
	} else {
		fprintf(stderr, "bluetooth_hid: %s sent an unreadable report map\n",
			Bluetooth::bdaddrUtils::ToString(map->address).String());
	}
}


void
BluetoothHIDInputDevice::_LowEnergyDisconnected(const bdaddr_t& address)
{
	BAutolock lock(fConnectionLock);

	for (int32 i = 0; i < kMaxConnections; i++) {
		hid_connection* connection = fConnections[i];
		if (connection == NULL || !connection->lowEnergy
			|| !Bluetooth::bdaddrUtils::Compare(connection->address,
				address)) {
			continue;
		}

		// Release whatever is still held, so nothing is left stuck down.
		uint8 empty[kBootKeyboardReportSize];
		memset(empty, 0, sizeof(empty));
		_KeyboardReport(connection, empty);
		if (connection->lastButtons != 0) {
			uint8 released[3] = { 0, 0, 0 };
			_MouseReport(connection, released, sizeof(released));
		}

		fConnections[i] = NULL;
		delete connection->reportMap;
		delete connection;
		return;
	}
}


status_t
BluetoothHIDInputDevice::_Connect(const bdaddr_t& address)
{
	BAutolock lock(fConnectionLock);

	int32 slot = -1;
	for (int32 i = 0; i < kMaxConnections; i++) {
		if (fConnections[i] == NULL) {
			if (slot < 0)
				slot = i;
		} else if (Bluetooth::bdaddrUtils::Compare(fConnections[i]->address,
				address)) {
			// Already connected to this device.
			return B_OK;
		}
	}

	if (slot < 0)
		return B_BUSY;

	ObjectDeleter<hid_connection> connection(new(std::nothrow) hid_connection);
	if (!connection.IsSet())
		return B_NO_MEMORY;

	memset(connection.Get(), 0, sizeof(hid_connection));
	connection->owner = this;
	connection->address = address;
	connection->control = -1;
	connection->interrupt = -1;
	connection->repeatDeadline = B_INFINITE_TIMEOUT;

	fConnections[slot] = connection.Get();

	connection->thread = spawn_thread(_ConnectionEntry,
		"bluetooth HID connection", B_URGENT_DISPLAY_PRIORITY,
		connection.Get());
	if (connection->thread < B_OK) {
		fConnections[slot] = NULL;
		return connection->thread;
	}

	// The thread owns the connection from here on.
	resume_thread(connection.Detach()->thread);
	return B_OK;
}


status_t
BluetoothHIDInputDevice::_ConnectionEntry(void* cookie)
{
	hid_connection* connection = (hid_connection*)cookie;
	connection->owner->_Connection(connection);
	return B_OK;
}


void
BluetoothHIDInputDevice::_Connection(hid_connection* connection)
{
	BString name = Bluetooth::bdaddrUtils::ToString(connection->address);

	connection->control = connect_l2cap(connection->address,
		L2CAP_PSM_HID_CTRL);
	if (connection->control < 0) {
		fprintf(stderr, "bluetooth_hid: %s: control channel failed: %s\n",
			name.String(), strerror(errno));
		_Disconnect(connection);
		return;
	}

	set_socket_timeout(connection->control, SO_RCVTIMEO, kHandshakeTimeout);

	if (send(connection->control, &kHIDPSetBootProtocol, 1, 0) != 1) {
		fprintf(stderr, "bluetooth_hid: %s: SET_PROTOCOL failed: %s\n",
			name.String(), strerror(errno));
		_Disconnect(connection);
		return;
	}

	uint8 handshake;
	if (recv(connection->control, &handshake, 1, 0) != 1
		|| handshake != kHIDPHandshakeSuccessful) {
		// Report protocol devices need a parsed report descriptor, which this
		// add-on does not implement, so they are rejected here rather than
		// guessed at from the report sizes.
		fprintf(stderr, "bluetooth_hid: %s: boot protocol not supported\n",
			name.String());
		_Disconnect(connection);
		return;
	}

	connection->interrupt = connect_l2cap(connection->address,
		L2CAP_PSM_HID_INT);
	if (connection->interrupt < 0) {
		fprintf(stderr, "bluetooth_hid: %s: interrupt channel failed: %s\n",
			name.String(), strerror(errno));
		_Disconnect(connection);
		return;
	}

	_ReadReports(connection);

	// Release whatever is still held down so that no key or button is stuck
	// after the device goes away.
	uint8 empty[kBootKeyboardReportSize];
	memset(empty, 0, sizeof(empty));
	_KeyboardReport(connection, empty);
	if (connection->lastButtons != 0) {
		uint8 released[3] = { 0, 0, 0 };
		_MouseReport(connection, released, sizeof(released));
	}

	_Disconnect(connection);
}


void
BluetoothHIDInputDevice::_Disconnect(hid_connection* connection)
{
	if (fConnectionLock.Lock()) {
		for (int32 i = 0; i < kMaxConnections; i++) {
			if (fConnections[i] == connection) {
				fConnections[i] = NULL;
				break;
			}
		}
		fConnectionLock.Unlock();
	}

	if (connection->interrupt >= 0)
		close(connection->interrupt);
	if (connection->control >= 0)
		close(connection->control);

	delete connection;
}


void
BluetoothHIDInputDevice::_ReadReports(hid_connection* connection)
{
	uint8 packet[64];
	bigtime_t currentTimeout = -1;

	while (fRunning) {
		bigtime_t timeout = kIdleReceiveTimeout;
		if (connection->repeatUsage != 0) {
			bigtime_t remaining = connection->repeatDeadline - system_time();
			if (remaining < 1000)
				remaining = 1000;
			if (remaining < timeout)
				timeout = remaining;
		}

		// Reports arrive far more often than the timeout changes, so don't
		// pay for a setsockopt() on every one of them.
		if (timeout != currentTimeout) {
			set_socket_timeout(connection->interrupt, SO_RCVTIMEO, timeout);
			currentTimeout = timeout;
		}

		ssize_t size = recv(connection->interrupt, packet, sizeof(packet), 0);
		if (size < 0) {
			if (errno != B_TIMED_OUT && errno != EWOULDBLOCK
				&& errno != EINTR) {
				break;
			}

			if (connection->repeatUsage != 0
				&& system_time() >= connection->repeatDeadline) {
				uint8 usage = connection->repeatUsage;
				_Key(connection, usage, true);
				// _Key() restarts the initial delay, the following repeats
				// come at the configured rate instead.
				connection->repeatUsage = usage;
				connection->repeatDeadline = system_time() + fRepeatRate;
			}
			continue;
		}

		if (size == 0)
			break;

		if (packet[0] != kHIDPDataInput || fStartedDevices == 0)
			continue;

		// Boot keyboards send an eight byte report, boot mice three bytes
		// plus an optional wheel byte.
		if ((size_t)size == kBootKeyboardReportSize + 1)
			_KeyboardReport(connection, packet + 1);
		else if (size >= 4 && size <= 5)
			_MouseReport(connection, packet + 1, size - 1);
	}
}


bool
BluetoothHIDInputDevice::_ContainsUsage(const uint8* report, uint8 usage)
{
	for (size_t i = 2; i < kBootKeyboardReportSize; i++) {
		if (report[i] == usage)
			return true;
	}

	return false;
}


void
BluetoothHIDInputDevice::_KeyboardReport(hid_connection* connection,
	const uint8* report)
{
	// A rollover error blanks the usage list, it does not mean that every key
	// was released.
	for (size_t i = 2; i < kBootKeyboardReportSize; i++) {
		if (report[i] == 0x01)
			return;
	}

	uint8 modifierChange = connection->lastReport[0] ^ report[0];
	for (int i = 0; i < 8; i++) {
		if ((modifierChange & (1 << i)) != 0)
			_Key(connection, 0xe0 + i, (report[0] & (1 << i)) != 0);
	}

	// Releases first, so that a report which swaps two keys at once does not
	// leave the new key marked as released.
	for (size_t i = 2; i < kBootKeyboardReportSize; i++) {
		uint8 usage = connection->lastReport[i];
		if (usage > 0x03 && !_ContainsUsage(report, usage))
			_Key(connection, usage, false);
	}

	for (size_t i = 2; i < kBootKeyboardReportSize; i++) {
		uint8 usage = report[i];
		if (usage > 0x03 && !_ContainsUsage(connection->lastReport, usage))
			_Key(connection, usage, true);
	}

	memcpy(connection->lastReport, report, kBootKeyboardReportSize);
}


void
BluetoothHIDInputDevice::_Key(hid_connection* connection, uint8 usage,
	bool down)
{
	uint32 keyCode = _KeyCode(usage);
	if (keyCode == 0 || keyCode >= 256)
		return;

	if (down)
		connection->states[keyCode >> 3] |= 1 << (7 - (keyCode & 0x7));
	else
		connection->states[keyCode >> 3] &= ~(1 << (7 - (keyCode & 0x7)));

	BAutolock lock(fKeymapLock);

	uint32 modifier = fKeymap.Modifier(keyCode);
	bool isLock
		= (modifier & (B_CAPS_LOCK | B_NUM_LOCK | B_SCROLL_LOCK)) != 0;
	if (modifier != 0 && (!isLock || down)) {
		uint32 oldModifiers = connection->modifiers;

		if ((down && !isLock)
			|| (down && (connection->modifiers & modifier) == 0)) {
			connection->modifiers |= modifier;
		} else {
			connection->modifiers &= ~modifier;

			// Don't clear a combined B_*_KEY while the opposite
			// B_{LEFT|RIGHT}_*_KEY is still held down.
			if ((connection->modifiers
					& (B_LEFT_SHIFT_KEY | B_RIGHT_SHIFT_KEY)) != 0)
				connection->modifiers |= B_SHIFT_KEY;
			if ((connection->modifiers
					& (B_LEFT_COMMAND_KEY | B_RIGHT_COMMAND_KEY)) != 0)
				connection->modifiers |= B_COMMAND_KEY;
			if ((connection->modifiers
					& (B_LEFT_CONTROL_KEY | B_RIGHT_CONTROL_KEY)) != 0)
				connection->modifiers |= B_CONTROL_KEY;
			if ((connection->modifiers
					& (B_LEFT_OPTION_KEY | B_RIGHT_OPTION_KEY)) != 0)
				connection->modifiers |= B_OPTION_KEY;
		}

		if (connection->modifiers != oldModifiers) {
			BMessage* message = new(std::nothrow) BMessage(
				B_MODIFIERS_CHANGED);
			if (message != NULL) {
				message->AddInt64("when", system_time());
				message->AddInt32("be:old_modifiers", oldModifiers);
				message->AddInt32("modifiers", connection->modifiers);
				message->AddData("states", B_UINT8_TYPE, connection->states,
					16);
				if (EnqueueMessage(message) != B_OK)
					delete message;
			}
		}
	}

	char* string = NULL;
	int32 numBytes = 0;
	fKeymap.GetChars(keyCode, connection->modifiers, 0, &string, &numBytes);
	ArrayDeleter<char> stringDeleter(string);

	char* rawString = NULL;
	int32 rawNumBytes = 0;
	fKeymap.GetChars(keyCode, 0, 0, &rawString, &rawNumBytes);
	ArrayDeleter<char> rawStringDeleter(rawString);

	BMessage* message = new(std::nothrow) BMessage();
	if (message == NULL)
		return;

	if (numBytes > 0)
		message->what = down ? B_KEY_DOWN : B_KEY_UP;
	else
		message->what = down ? B_UNMAPPED_KEY_DOWN : B_UNMAPPED_KEY_UP;

	message->AddInt64("when", system_time());
	message->AddInt32("key", keyCode);
	message->AddInt32("modifiers", connection->modifiers);
	message->AddData("states", B_UINT8_TYPE, connection->states, 16);

	if (numBytes > 0) {
		for (int32 i = 0; i < numBytes; i++)
			message->AddInt8("byte", (int8)string[i]);
		message->AddData("bytes", B_STRING_TYPE, string, numBytes + 1);

		if (down && connection->lastKeyCode == keyCode)
			message->AddInt32("be:key_repeat", ++connection->repeatCount);
		else
			connection->repeatCount = 1;
	}

	if (rawNumBytes > 0)
		message->AddInt32("raw_char", (uint32)((uint8)rawString[0] & 0x7f));
	else if (numBytes > 0)
		message->AddInt32("raw_char", (uint32)((uint8)string[0] & 0x7f));

	connection->lastKeyCode = down ? keyCode : 0;

	// Only keys that produce characters take part in auto repeat, repeating a
	// modifier would just resend it forever.
	if (down && numBytes > 0) {
		connection->repeatUsage = usage;
		connection->repeatDeadline = system_time() + fRepeatDelay;
	} else if (connection->repeatUsage == usage) {
		connection->repeatUsage = 0;
		connection->repeatDeadline = B_INFINITE_TIMEOUT;
	}

	if (EnqueueMessage(message) != B_OK)
		delete message;
}


void
BluetoothHIDInputDevice::_MouseReport(hid_connection* connection,
	const uint8* report, size_t size)
{
	if (size < 3)
		return;

	uint32 rawButtons = report[0] & 0x07;
	uint32 buttons = _RemapButtons(rawButtons);

	int32 x = (int8)report[1];
	// HID reports Y growing downwards, the input server expects it to grow
	// upwards.
	int32 y = -(int32)(int8)report[2];
	int32 wheel = size >= 4 ? (int8)report[3] : 0;

	int32 deltaX;
	int32 deltaY;
	_ComputeAcceleration(connection, x, y, deltaX, deltaY);

	uint32 changed = connection->lastButtons ^ rawButtons;
	if (changed != 0) {
		bool pressed = (changed & rawButtons) != 0;

		if (pressed) {
			// Two presses of the same button inside the configured interval
			// are a double click; anything else starts the count again.
			bigtime_t now = system_time();
			if (connection->lastClickButtons == buttons
				&& now - connection->lastClickTime
					< fMouseSettings.click_speed) {
				connection->clickCount++;
			} else
				connection->clickCount = 1;

			connection->lastClickTime = now;
			connection->lastClickButtons = buttons;
		}

		BMessage* message = new(std::nothrow) BMessage(
			pressed ? B_MOUSE_DOWN : B_MOUSE_UP);
		if (message != NULL) {
			message->AddInt64("when", system_time());
			message->AddInt32("buttons", buttons);
			message->AddInt32("x", deltaX);
			message->AddInt32("y", deltaY);
			message->AddInt32("be:device_subtype", B_MOUSE_POINTING_DEVICE);
			if (pressed)
				message->AddInt32("clicks", connection->clickCount);
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}
	}

	if (deltaX != 0 || deltaY != 0) {
		BMessage* message = new(std::nothrow) BMessage(B_MOUSE_MOVED);
		if (message != NULL) {
			message->AddInt64("when", system_time());
			message->AddInt32("buttons", buttons);
			message->AddInt32("x", deltaX);
			message->AddInt32("y", deltaY);
			message->AddInt32("be:device_subtype", B_MOUSE_POINTING_DEVICE);
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}
	}

	if (wheel != 0) {
		BMessage* message = new(std::nothrow) BMessage(
			B_MOUSE_WHEEL_CHANGED);
		if (message != NULL) {
			message->AddInt64("when", system_time());
			message->AddFloat("be:wheel_delta_x", 0);
			// HID reports the wheel growing away from the user, the input
			// server expects a positive delta to scroll down.
			message->AddFloat("be:wheel_delta_y", -wheel);
			if (EnqueueMessage(message) != B_OK)
				delete message;
		}
	}

	connection->lastButtons = (uint8)rawButtons;
}


uint32
BluetoothHIDInputDevice::_KeyCode(uint8 usage)
{
	// HID keyboard usages 0x04 to 0x66 mapped to Haiku key codes.
	static const uint8 kKeyCodes[] = {
		0x3c, 0x50, 0x4e, 0x3e, 0x29, 0x3f, 0x40, 0x41, 0x2e, 0x42, 0x43,
		0x44, 0x52, 0x51, 0x2f, 0x30, 0x27, 0x2a, 0x3d, 0x2b, 0x2d, 0x4f,
		0x28, 0x4d, 0x2c, 0x4c, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
		0x19, 0x1a, 0x1b, 0x47, 0x01, 0x1e, 0x26, 0x5e, 0x1c, 0x1d, 0x31,
		0x32, 0x33, 0x33, 0x45, 0x46, 0x11, 0x53, 0x54, 0x55,
		B_CAPS_LOCK_KEY, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
		0x0a, 0x0b, 0x0c, 0x0d, 0x0e, B_SCROLL_KEY, B_PAUSE_KEY, 0x1f,
		0x20, 0x21, 0x34, 0x35, 0x36, 0x63, 0x61, 0x62, 0x57, 0x22, 0x23,
		0x24, 0x25, 0x3a, 0x5b, 0x58, 0x59, 0x5a, 0x48, 0x49, 0x4a, 0x37,
		0x38, 0x39, 0x64, 0x65, 0x69, 0x68
	};

	if (usage >= 0x04 && usage < 0x04 + sizeof(kKeyCodes))
		return kKeyCodes[usage - 0x04];

	// The eight modifier usages, left to right control, shift, alt and GUI.
	static const uint8 kModifierCodes[]
		= { 0x5c, 0x4b, 0x5d, 0x66, 0x60, 0x56, 0x5f, 0x67 };

	if (usage >= 0xe0 && usage <= 0xe7)
		return kModifierCodes[usage - 0xe0];

	return 0;
}


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new(std::nothrow) BluetoothHIDInputDevice();
}
