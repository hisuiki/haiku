#pragma once
#include "WlResource.h"


class XdgWmBase: public WlResource {
public:
	virtual ~XdgWmBase() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgWmBase *FromResource(struct wl_resource *resource) {return (XdgWmBase*)WlResource::FromResource(resource);}

	enum Error {
		errorRole = 0,
		errorDefunctSurfaces = 1,
		errorNotTheTopmostPopup = 2,
		errorInvalidPopupParent = 3,
		errorInvalidSurfaceState = 4,
		errorInvalidPositioner = 5,
		errorUnresponsive = 6,
	};

	virtual void HandleDestroy();
	virtual void HandleCreatePositioner(uint32_t id) = 0;
	virtual void HandleGetXdgSurface(uint32_t id, struct wl_resource *surface) = 0;
	virtual void HandlePong(uint32_t serial) = 0;

	void SendPing(uint32_t serial) {wl_resource_post_event(ToResource(), 0, serial);}
};

class XdgPositioner: public WlResource {
public:
	virtual ~XdgPositioner() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgPositioner *FromResource(struct wl_resource *resource) {return (XdgPositioner*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidInput = 0,
	};

	enum Anchor {
		anchorNone = 0,
		anchorTop = 1,
		anchorBottom = 2,
		anchorLeft = 3,
		anchorRight = 4,
		anchorTopLeft = 5,
		anchorBottomLeft = 6,
		anchorTopRight = 7,
		anchorBottomRight = 8,
	};

	enum Gravity {
		gravityNone = 0,
		gravityTop = 1,
		gravityBottom = 2,
		gravityLeft = 3,
		gravityRight = 4,
		gravityTopLeft = 5,
		gravityBottomLeft = 6,
		gravityTopRight = 7,
		gravityBottomRight = 8,
	};

	enum ConstraintAdjustment {
		constraintAdjustmentNone = 0,
		constraintAdjustmentSlideX = 1,
		constraintAdjustmentSlideY = 2,
		constraintAdjustmentFlipX = 4,
		constraintAdjustmentFlipY = 8,
		constraintAdjustmentResizeX = 16,
		constraintAdjustmentResizeY = 32,
	};

	virtual void HandleDestroy();
	virtual void HandleSetSize(int32_t width, int32_t height) = 0;
	virtual void HandleSetAnchorRect(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleSetAnchor(uint32_t anchor) = 0;
	virtual void HandleSetGravity(uint32_t gravity) = 0;
	virtual void HandleSetConstraintAdjustment(uint32_t constraint_adjustment) = 0;
	virtual void HandleSetOffset(int32_t x, int32_t y) = 0;
	virtual void HandleSetReactive() = 0;
	virtual void HandleSetParentSize(int32_t parent_width, int32_t parent_height) = 0;
	virtual void HandleSetParentConfigure(uint32_t serial) = 0;
};

class XdgSurface: public WlResource {
public:
	virtual ~XdgSurface() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgSurface *FromResource(struct wl_resource *resource) {return (XdgSurface*)WlResource::FromResource(resource);}

	enum Error {
		errorNotConstructed = 1,
		errorAlreadyConstructed = 2,
		errorUnconfiguredBuffer = 3,
		errorInvalidSerial = 4,
		errorInvalidSize = 5,
		errorDefunctRoleObject = 6,
	};

	virtual void HandleDestroy();
	virtual void HandleGetToplevel(uint32_t id) = 0;
	virtual void HandleGetPopup(uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) = 0;
	virtual void HandleSetWindowGeometry(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleAckConfigure(uint32_t serial) = 0;

	void SendConfigure(uint32_t serial) {wl_resource_post_event(ToResource(), 0, serial);}
};

class XdgToplevel: public WlResource {
public:
	virtual ~XdgToplevel() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgToplevel *FromResource(struct wl_resource *resource) {return (XdgToplevel*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidResizeEdge = 0,
		errorInvalidParent = 1,
		errorInvalidSize = 2,
	};

	enum ResizeEdge {
		resizeEdgeNone = 0,
		resizeEdgeTop = 1,
		resizeEdgeBottom = 2,
		resizeEdgeLeft = 4,
		resizeEdgeTopLeft = 5,
		resizeEdgeBottomLeft = 6,
		resizeEdgeRight = 8,
		resizeEdgeTopRight = 9,
		resizeEdgeBottomRight = 10,
	};

	enum State {
		stateMaximized = 1,
		stateFullscreen = 2,
		stateResizing = 3,
		stateActivated = 4,
		stateTiledLeft = 5,
		stateTiledRight = 6,
		stateTiledTop = 7,
		stateTiledBottom = 8,
		stateSuspended = 9,
	};

	enum WmCapabilities {
		wmCapabilitiesWindowMenu = 1,
		wmCapabilitiesMaximize = 2,
		wmCapabilitiesFullscreen = 3,
		wmCapabilitiesMinimize = 4,
	};

	virtual void HandleDestroy();
	virtual void HandleSetParent(struct wl_resource *parent) = 0;
	virtual void HandleSetTitle(const char *title) = 0;
	virtual void HandleSetAppId(const char *app_id) = 0;
	virtual void HandleShowWindowMenu(struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) = 0;
	virtual void HandleMove(struct wl_resource *seat, uint32_t serial) = 0;
	virtual void HandleResize(struct wl_resource *seat, uint32_t serial, uint32_t edges) = 0;
	virtual void HandleSetMaxSize(int32_t width, int32_t height) = 0;
	virtual void HandleSetMinSize(int32_t width, int32_t height) = 0;
	virtual void HandleSetMaximized() = 0;
	virtual void HandleUnsetMaximized() = 0;
	virtual void HandleSetFullscreen(struct wl_resource *output) = 0;
	virtual void HandleUnsetFullscreen() = 0;
	virtual void HandleSetMinimized() = 0;

	void SendConfigure(int32_t width, int32_t height, struct wl_array *states) {wl_resource_post_event(ToResource(), 0, width, height, states);}
	void SendClose() {wl_resource_post_event(ToResource(), 1);}
	void SendConfigureBounds(int32_t width, int32_t height) {wl_resource_post_event(ToResource(), 2, width, height);}
	void SendWmCapabilities(struct wl_array *capabilities) {wl_resource_post_event(ToResource(), 3, capabilities);}
};

class XdgPopup: public WlResource {
public:
	virtual ~XdgPopup() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgPopup *FromResource(struct wl_resource *resource) {return (XdgPopup*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidGrab = 0,
	};

	virtual void HandleDestroy();
	virtual void HandleGrab(struct wl_resource *seat, uint32_t serial) = 0;
	virtual void HandleReposition(struct wl_resource *positioner, uint32_t token) = 0;

	void SendConfigure(int32_t x, int32_t y, int32_t width, int32_t height) {wl_resource_post_event(ToResource(), 0, x, y, width, height);}
	void SendPopupDone() {wl_resource_post_event(ToResource(), 1);}
	void SendRepositioned(uint32_t token) {wl_resource_post_event(ToResource(), 2, token);}
};
