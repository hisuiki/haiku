#pragma once
#include "WlResource.h"


class WlDisplay: public WlResource {
public:
	virtual ~WlDisplay() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlDisplay *FromResource(struct wl_resource *resource) {return (WlDisplay*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidObject = 0,
		errorInvalidMethod = 1,
		errorNoMemory = 2,
		errorImplementation = 3,
	};

	virtual void HandleSync(uint32_t callback) = 0;
	virtual void HandleGetRegistry(uint32_t registry) = 0;

	void SendError(struct wl_resource *object_id, uint32_t code, const char *message) {wl_resource_post_event(ToResource(), 0, object_id, code, message);}
	void SendDeleteId(uint32_t id) {wl_resource_post_event(ToResource(), 1, id);}
};

class WlRegistry: public WlResource {
public:
	virtual ~WlRegistry() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlRegistry *FromResource(struct wl_resource *resource) {return (WlRegistry*)WlResource::FromResource(resource);}

	virtual void HandleBind(uint32_t name, const char *interface, uint32_t version, uint32_t id) = 0;

	void SendGlobal(uint32_t name, const char *interface, uint32_t version) {wl_resource_post_event(ToResource(), 0, name, interface, version);}
	void SendGlobalRemove(uint32_t name) {wl_resource_post_event(ToResource(), 1, name);}
};

class WlCallback: public WlResource {
public:
	virtual ~WlCallback() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlCallback *FromResource(struct wl_resource *resource) {return (WlCallback*)WlResource::FromResource(resource);}

	void SendDone(uint32_t callback_data) {wl_resource_post_event(ToResource(), 0, callback_data);}
};

class WlCompositor: public WlResource {
public:
	virtual ~WlCompositor() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlCompositor *FromResource(struct wl_resource *resource) {return (WlCompositor*)WlResource::FromResource(resource);}

	virtual void HandleCreateSurface(uint32_t id) = 0;
	virtual void HandleCreateRegion(uint32_t id) = 0;
};

class WlShmPool: public WlResource {
public:
	virtual ~WlShmPool() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlShmPool *FromResource(struct wl_resource *resource) {return (WlShmPool*)WlResource::FromResource(resource);}

	virtual void HandleCreateBuffer(uint32_t id, int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format) = 0;
	virtual void HandleDestroy();
	virtual void HandleResize(int32_t size) = 0;
};

class WlShm: public WlResource {
public:
	virtual ~WlShm() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlShm *FromResource(struct wl_resource *resource) {return (WlShm*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidFormat = 0,
		errorInvalidStride = 1,
		errorInvalidFd = 2,
	};

	enum Format {
		formatArgb8888 = 0,
		formatXrgb8888 = 1,
		formatC8 = 0x20203843,
		formatRgb332 = 0x38424752,
		formatBgr233 = 0x38524742,
		formatXrgb4444 = 0x32315258,
		formatXbgr4444 = 0x32314258,
		formatRgbx4444 = 0x32315852,
		formatBgrx4444 = 0x32315842,
		formatArgb4444 = 0x32315241,
		formatAbgr4444 = 0x32314241,
		formatRgba4444 = 0x32314152,
		formatBgra4444 = 0x32314142,
		formatXrgb1555 = 0x35315258,
		formatXbgr1555 = 0x35314258,
		formatRgbx5551 = 0x35315852,
		formatBgrx5551 = 0x35315842,
		formatArgb1555 = 0x35315241,
		formatAbgr1555 = 0x35314241,
		formatRgba5551 = 0x35314152,
		formatBgra5551 = 0x35314142,
		formatRgb565 = 0x36314752,
		formatBgr565 = 0x36314742,
		formatRgb888 = 0x34324752,
		formatBgr888 = 0x34324742,
		formatXbgr8888 = 0x34324258,
		formatRgbx8888 = 0x34325852,
		formatBgrx8888 = 0x34325842,
		formatAbgr8888 = 0x34324241,
		formatRgba8888 = 0x34324152,
		formatBgra8888 = 0x34324142,
		formatXrgb2101010 = 0x30335258,
		formatXbgr2101010 = 0x30334258,
		formatRgbx1010102 = 0x30335852,
		formatBgrx1010102 = 0x30335842,
		formatArgb2101010 = 0x30335241,
		formatAbgr2101010 = 0x30334241,
		formatRgba1010102 = 0x30334152,
		formatBgra1010102 = 0x30334142,
		formatYuyv = 0x56595559,
		formatYvyu = 0x55595659,
		formatUyvy = 0x59565955,
		formatVyuy = 0x59555956,
		formatAyuv = 0x56555941,
		formatNv12 = 0x3231564e,
		formatNv21 = 0x3132564e,
		formatNv16 = 0x3631564e,
		formatNv61 = 0x3136564e,
		formatYuv410 = 0x39565559,
		formatYvu410 = 0x39555659,
		formatYuv411 = 0x31315559,
		formatYvu411 = 0x31315659,
		formatYuv420 = 0x32315559,
		formatYvu420 = 0x32315659,
		formatYuv422 = 0x36315559,
		formatYvu422 = 0x36315659,
		formatYuv444 = 0x34325559,
		formatYvu444 = 0x34325659,
		formatR8 = 0x20203852,
		formatR16 = 0x20363152,
		formatRg88 = 0x38384752,
		formatGr88 = 0x38385247,
		formatRg1616 = 0x32334752,
		formatGr1616 = 0x32335247,
		formatXrgb16161616f = 0x48345258,
		formatXbgr16161616f = 0x48344258,
		formatArgb16161616f = 0x48345241,
		formatAbgr16161616f = 0x48344241,
		formatXyuv8888 = 0x56555958,
		formatVuy888 = 0x34325556,
		formatVuy101010 = 0x30335556,
		formatY210 = 0x30313259,
		formatY212 = 0x32313259,
		formatY216 = 0x36313259,
		formatY410 = 0x30313459,
		formatY412 = 0x32313459,
		formatY416 = 0x36313459,
		formatXvyu2101010 = 0x30335658,
		formatXvyu12_16161616 = 0x36335658,
		formatXvyu16161616 = 0x38345658,
		formatY0l0 = 0x304c3059,
		formatX0l0 = 0x304c3058,
		formatY0l2 = 0x324c3059,
		formatX0l2 = 0x324c3058,
		formatYuv420_8bit = 0x38305559,
		formatYuv420_10bit = 0x30315559,
		formatXrgb8888A8 = 0x38415258,
		formatXbgr8888A8 = 0x38414258,
		formatRgbx8888A8 = 0x38415852,
		formatBgrx8888A8 = 0x38415842,
		formatRgb888A8 = 0x38413852,
		formatBgr888A8 = 0x38413842,
		formatRgb565A8 = 0x38413552,
		formatBgr565A8 = 0x38413542,
		formatNv24 = 0x3432564e,
		formatNv42 = 0x3234564e,
		formatP210 = 0x30313250,
		formatP010 = 0x30313050,
		formatP012 = 0x32313050,
		formatP016 = 0x36313050,
		formatAxbxgxrx106106106106 = 0x30314241,
		formatNv15 = 0x3531564e,
		formatQ410 = 0x30313451,
		formatQ401 = 0x31303451,
		formatXrgb16161616 = 0x38345258,
		formatXbgr16161616 = 0x38344258,
		formatArgb16161616 = 0x38345241,
		formatAbgr16161616 = 0x38344241,
		formatC1 = 0x20203143,
		formatC2 = 0x20203243,
		formatC4 = 0x20203443,
		formatD1 = 0x20203144,
		formatD2 = 0x20203244,
		formatD4 = 0x20203444,
		formatD8 = 0x20203844,
		formatR1 = 0x20203152,
		formatR2 = 0x20203252,
		formatR4 = 0x20203452,
		formatR10 = 0x20303152,
		formatR12 = 0x20323152,
		formatAvuy8888 = 0x59555641,
		formatXvuy8888 = 0x59555658,
		formatP030 = 0x30333050,
	};

	virtual void HandleCreatePool(uint32_t id, int32_t fd, int32_t size) = 0;
	virtual void HandleRelease();

	void SendFormat(uint32_t format) {wl_resource_post_event(ToResource(), 0, format);}
};

class WlBuffer: public WlResource {
public:
	virtual ~WlBuffer() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlBuffer *FromResource(struct wl_resource *resource) {return (WlBuffer*)WlResource::FromResource(resource);}

	virtual void HandleDestroy();

	void SendRelease() {wl_resource_post_event(ToResource(), 0);}
};

class WlDataOffer: public WlResource {
public:
	virtual ~WlDataOffer() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlDataOffer *FromResource(struct wl_resource *resource) {return (WlDataOffer*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidFinish = 0,
		errorInvalidActionMask = 1,
		errorInvalidAction = 2,
		errorInvalidOffer = 3,
	};

	virtual void HandleAccept(uint32_t serial, const char *mime_type) = 0;
	virtual void HandleReceive(const char *mime_type, int32_t fd) = 0;
	virtual void HandleDestroy();
	virtual void HandleFinish() = 0;
	virtual void HandleSetActions(uint32_t dnd_actions, uint32_t preferred_action) = 0;

	void SendOffer(const char *mime_type) {wl_resource_post_event(ToResource(), 0, mime_type);}
	void SendSourceActions(uint32_t source_actions) {wl_resource_post_event(ToResource(), 1, source_actions);}
	void SendAction(uint32_t dnd_action) {wl_resource_post_event(ToResource(), 2, dnd_action);}
};

class WlDataSource: public WlResource {
public:
	virtual ~WlDataSource() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlDataSource *FromResource(struct wl_resource *resource) {return (WlDataSource*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidActionMask = 0,
		errorInvalidSource = 1,
	};

	virtual void HandleOffer(const char *mime_type) = 0;
	virtual void HandleDestroy();
	virtual void HandleSetActions(uint32_t dnd_actions) = 0;

	void SendTarget(const char *mime_type) {wl_resource_post_event(ToResource(), 0, mime_type);}
	void SendSend(const char *mime_type, int32_t fd) {wl_resource_post_event(ToResource(), 1, mime_type, fd);}
	void SendCancelled() {wl_resource_post_event(ToResource(), 2);}
	void SendDndDropPerformed() {wl_resource_post_event(ToResource(), 3);}
	void SendDndFinished() {wl_resource_post_event(ToResource(), 4);}
	void SendAction(uint32_t dnd_action) {wl_resource_post_event(ToResource(), 5, dnd_action);}
};

class WlDataDevice: public WlResource {
public:
	virtual ~WlDataDevice() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlDataDevice *FromResource(struct wl_resource *resource) {return (WlDataDevice*)WlResource::FromResource(resource);}

	enum Error {
		errorRole = 0,
		errorUsedSource = 1,
	};

	virtual void HandleStartDrag(struct wl_resource *source, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) = 0;
	virtual void HandleSetSelection(struct wl_resource *source, uint32_t serial) = 0;
	virtual void HandleRelease();

	void SendDataOffer(struct wl_resource *id) {wl_resource_post_event(ToResource(), 0, id);}
	void SendEnter(uint32_t serial, struct wl_resource *surface, wl_fixed_t x, wl_fixed_t y, struct wl_resource *id) {wl_resource_post_event(ToResource(), 1, serial, surface, x, y, id);}
	void SendLeave() {wl_resource_post_event(ToResource(), 2);}
	void SendMotion(uint32_t time, wl_fixed_t x, wl_fixed_t y) {wl_resource_post_event(ToResource(), 3, time, x, y);}
	void SendDrop() {wl_resource_post_event(ToResource(), 4);}
	void SendSelection(struct wl_resource *id) {wl_resource_post_event(ToResource(), 5, id);}
};

class WlDataDeviceManager: public WlResource {
public:
	virtual ~WlDataDeviceManager() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlDataDeviceManager *FromResource(struct wl_resource *resource) {return (WlDataDeviceManager*)WlResource::FromResource(resource);}

	enum DndAction {
		dndActionNone = 0,
		dndActionCopy = 1,
		dndActionMove = 2,
		dndActionAsk = 4,
	};

	virtual void HandleCreateDataSource(uint32_t id) = 0;
	virtual void HandleGetDataDevice(uint32_t id, struct wl_resource *seat) = 0;
};

class WlShell: public WlResource {
public:
	virtual ~WlShell() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlShell *FromResource(struct wl_resource *resource) {return (WlShell*)WlResource::FromResource(resource);}

	enum Error {
		errorRole = 0,
	};

	virtual void HandleGetShellSurface(uint32_t id, struct wl_resource *surface) = 0;
};

class WlShellSurface: public WlResource {
public:
	virtual ~WlShellSurface() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlShellSurface *FromResource(struct wl_resource *resource) {return (WlShellSurface*)WlResource::FromResource(resource);}

	enum Resize {
		resizeNone = 0,
		resizeTop = 1,
		resizeBottom = 2,
		resizeLeft = 4,
		resizeTopLeft = 5,
		resizeBottomLeft = 6,
		resizeRight = 8,
		resizeTopRight = 9,
		resizeBottomRight = 10,
	};

	enum Transient {
		transientInactive = 0x1,
	};

	enum FullscreenMethod {
		fullscreenMethodDefault = 0,
		fullscreenMethodScale = 1,
		fullscreenMethodDriver = 2,
		fullscreenMethodFill = 3,
	};

	virtual void HandlePong(uint32_t serial) = 0;
	virtual void HandleMove(struct wl_resource *seat, uint32_t serial) = 0;
	virtual void HandleResize(struct wl_resource *seat, uint32_t serial, uint32_t edges) = 0;
	virtual void HandleSetToplevel() = 0;
	virtual void HandleSetTransient(struct wl_resource *parent, int32_t x, int32_t y, uint32_t flags) = 0;
	virtual void HandleSetFullscreen(uint32_t method, uint32_t framerate, struct wl_resource *output) = 0;
	virtual void HandleSetPopup(struct wl_resource *seat, uint32_t serial, struct wl_resource *parent, int32_t x, int32_t y, uint32_t flags) = 0;
	virtual void HandleSetMaximized(struct wl_resource *output) = 0;
	virtual void HandleSetTitle(const char *title) = 0;
	virtual void HandleSetClass(const char *class_) = 0;

	void SendPing(uint32_t serial) {wl_resource_post_event(ToResource(), 0, serial);}
	void SendConfigure(uint32_t edges, int32_t width, int32_t height) {wl_resource_post_event(ToResource(), 1, edges, width, height);}
	void SendPopupDone() {wl_resource_post_event(ToResource(), 2);}
};

class WlSurface: public WlResource {
public:
	virtual ~WlSurface() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlSurface *FromResource(struct wl_resource *resource) {return (WlSurface*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidScale = 0,
		errorInvalidTransform = 1,
		errorInvalidSize = 2,
		errorInvalidOffset = 3,
		errorDefunctRoleObject = 4,
	};

	virtual void HandleDestroy();
	virtual void HandleAttach(struct wl_resource *buffer, int32_t x, int32_t y) = 0;
	virtual void HandleDamage(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleFrame(uint32_t callback) = 0;
	virtual void HandleSetOpaqueRegion(struct wl_resource *region) = 0;
	virtual void HandleSetInputRegion(struct wl_resource *region) = 0;
	virtual void HandleCommit() = 0;
	virtual void HandleSetBufferTransform(int32_t transform) = 0;
	virtual void HandleSetBufferScale(int32_t scale) = 0;
	virtual void HandleDamageBuffer(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleOffset(int32_t x, int32_t y) = 0;

	void SendEnter(struct wl_resource *output) {wl_resource_post_event(ToResource(), 0, output);}
	void SendLeave(struct wl_resource *output) {wl_resource_post_event(ToResource(), 1, output);}
	void SendPreferredBufferScale(int32_t factor) {wl_resource_post_event(ToResource(), 2, factor);}
	void SendPreferredBufferTransform(uint32_t transform) {wl_resource_post_event(ToResource(), 3, transform);}
};

class WlSeat: public WlResource {
public:
	virtual ~WlSeat() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlSeat *FromResource(struct wl_resource *resource) {return (WlSeat*)WlResource::FromResource(resource);}

	enum Capability {
		capabilityPointer = 1,
		capabilityKeyboard = 2,
		capabilityTouch = 4,
	};

	enum Error {
		errorMissingCapability = 0,
	};

	virtual void HandleGetPointer(uint32_t id) = 0;
	virtual void HandleGetKeyboard(uint32_t id) = 0;
	virtual void HandleGetTouch(uint32_t id) = 0;
	virtual void HandleRelease();

	void SendCapabilities(uint32_t capabilities) {wl_resource_post_event(ToResource(), 0, capabilities);}
	void SendName(const char *name) {wl_resource_post_event(ToResource(), 1, name);}
};

class WlPointer: public WlResource {
public:
	virtual ~WlPointer() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlPointer *FromResource(struct wl_resource *resource) {return (WlPointer*)WlResource::FromResource(resource);}

	enum Error {
		errorRole = 0,
	};

	enum ButtonState {
		buttonStateReleased = 0,
		buttonStatePressed = 1,
	};

	enum Axis {
		axisVerticalScroll = 0,
		axisHorizontalScroll = 1,
	};

	enum AxisSource {
		axisSourceWheel = 0,
		axisSourceFinger = 1,
		axisSourceContinuous = 2,
		axisSourceWheelTilt = 3,
	};

	enum AxisRelativeDirection {
		axisRelativeDirectionIdentical = 0,
		axisRelativeDirectionInverted = 1,
	};

	virtual void HandleSetCursor(uint32_t serial, struct wl_resource *surface, int32_t hotspot_x, int32_t hotspot_y) = 0;
	virtual void HandleRelease();

	void SendEnter(uint32_t serial, struct wl_resource *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {wl_resource_post_event(ToResource(), 0, serial, surface, surface_x, surface_y);}
	void SendLeave(uint32_t serial, struct wl_resource *surface) {wl_resource_post_event(ToResource(), 1, serial, surface);}
	void SendMotion(uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {wl_resource_post_event(ToResource(), 2, time, surface_x, surface_y);}
	void SendButton(uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {wl_resource_post_event(ToResource(), 3, serial, time, button, state);}
	void SendAxis(uint32_t time, uint32_t axis, wl_fixed_t value) {wl_resource_post_event(ToResource(), 4, time, axis, value);}
	void SendFrame() {wl_resource_post_event(ToResource(), 5);}
	void SendAxisSource(uint32_t axis_source) {wl_resource_post_event(ToResource(), 6, axis_source);}
	void SendAxisStop(uint32_t time, uint32_t axis) {wl_resource_post_event(ToResource(), 7, time, axis);}
	void SendAxisDiscrete(uint32_t axis, int32_t discrete) {wl_resource_post_event(ToResource(), 8, axis, discrete);}
	void SendAxisValue120(uint32_t axis, int32_t value120) {wl_resource_post_event(ToResource(), 9, axis, value120);}
	void SendAxisRelativeDirection(uint32_t axis, uint32_t direction) {wl_resource_post_event(ToResource(), 10, axis, direction);}
};

class WlKeyboard: public WlResource {
public:
	virtual ~WlKeyboard() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlKeyboard *FromResource(struct wl_resource *resource) {return (WlKeyboard*)WlResource::FromResource(resource);}

	enum KeymapFormat {
		keymapFormatNoKeymap = 0,
		keymapFormatXkbV1 = 1,
	};

	enum KeyState {
		keyStateReleased = 0,
		keyStatePressed = 1,
	};

	virtual void HandleRelease();

	void SendKeymap(uint32_t format, int32_t fd, uint32_t size) {wl_resource_post_event(ToResource(), 0, format, fd, size);}
	void SendEnter(uint32_t serial, struct wl_resource *surface, struct wl_array *keys) {wl_resource_post_event(ToResource(), 1, serial, surface, keys);}
	void SendLeave(uint32_t serial, struct wl_resource *surface) {wl_resource_post_event(ToResource(), 2, serial, surface);}
	void SendKey(uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {wl_resource_post_event(ToResource(), 3, serial, time, key, state);}
	void SendModifiers(uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {wl_resource_post_event(ToResource(), 4, serial, mods_depressed, mods_latched, mods_locked, group);}
	void SendRepeatInfo(int32_t rate, int32_t delay) {wl_resource_post_event(ToResource(), 5, rate, delay);}
};

class WlTouch: public WlResource {
public:
	virtual ~WlTouch() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlTouch *FromResource(struct wl_resource *resource) {return (WlTouch*)WlResource::FromResource(resource);}

	virtual void HandleRelease();

	void SendDown(uint32_t serial, uint32_t time, struct wl_resource *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {wl_resource_post_event(ToResource(), 0, serial, time, surface, id, x, y);}
	void SendUp(uint32_t serial, uint32_t time, int32_t id) {wl_resource_post_event(ToResource(), 1, serial, time, id);}
	void SendMotion(uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {wl_resource_post_event(ToResource(), 2, time, id, x, y);}
	void SendFrame() {wl_resource_post_event(ToResource(), 3);}
	void SendCancel() {wl_resource_post_event(ToResource(), 4);}
	void SendShape(int32_t id, wl_fixed_t major, wl_fixed_t minor) {wl_resource_post_event(ToResource(), 5, id, major, minor);}
	void SendOrientation(int32_t id, wl_fixed_t orientation) {wl_resource_post_event(ToResource(), 6, id, orientation);}
};

class WlOutput: public WlResource {
public:
	virtual ~WlOutput() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlOutput *FromResource(struct wl_resource *resource) {return (WlOutput*)WlResource::FromResource(resource);}

	enum Subpixel {
		subpixelUnknown = 0,
		subpixelNone = 1,
		subpixelHorizontalRgb = 2,
		subpixelHorizontalBgr = 3,
		subpixelVerticalRgb = 4,
		subpixelVerticalBgr = 5,
	};

	enum Transform {
		transformNormal = 0,
		transform90 = 1,
		transform180 = 2,
		transform270 = 3,
		transformFlipped = 4,
		transformFlipped90 = 5,
		transformFlipped180 = 6,
		transformFlipped270 = 7,
	};

	enum Mode {
		modeCurrent = 0x1,
		modePreferred = 0x2,
	};

	virtual void HandleRelease();

	void SendGeometry(int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {wl_resource_post_event(ToResource(), 0, x, y, physical_width, physical_height, subpixel, make, model, transform);}
	void SendMode(uint32_t flags, int32_t width, int32_t height, int32_t refresh) {wl_resource_post_event(ToResource(), 1, flags, width, height, refresh);}
	void SendDone() {wl_resource_post_event(ToResource(), 2);}
	void SendScale(int32_t factor) {wl_resource_post_event(ToResource(), 3, factor);}
	void SendName(const char *name) {wl_resource_post_event(ToResource(), 4, name);}
	void SendDescription(const char *description) {wl_resource_post_event(ToResource(), 5, description);}
};

class WlRegion: public WlResource {
public:
	virtual ~WlRegion() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlRegion *FromResource(struct wl_resource *resource) {return (WlRegion*)WlResource::FromResource(resource);}

	virtual void HandleDestroy();
	virtual void HandleAdd(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleSubtract(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
};

class WlSubcompositor: public WlResource {
public:
	virtual ~WlSubcompositor() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlSubcompositor *FromResource(struct wl_resource *resource) {return (WlSubcompositor*)WlResource::FromResource(resource);}

	enum Error {
		errorBadSurface = 0,
		errorBadParent = 1,
	};

	virtual void HandleDestroy();
	virtual void HandleGetSubsurface(uint32_t id, struct wl_resource *surface, struct wl_resource *parent) = 0;
};

class WlSubsurface: public WlResource {
public:
	virtual ~WlSubsurface() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WlSubsurface *FromResource(struct wl_resource *resource) {return (WlSubsurface*)WlResource::FromResource(resource);}

	enum Error {
		errorBadSurface = 0,
	};

	virtual void HandleDestroy();
	virtual void HandleSetPosition(int32_t x, int32_t y) = 0;
	virtual void HandlePlaceAbove(struct wl_resource *sibling) = 0;
	virtual void HandlePlaceBelow(struct wl_resource *sibling) = 0;
	virtual void HandleSetSync() = 0;
	virtual void HandleSetDesync() = 0;
};
