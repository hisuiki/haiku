#pragma once
#include "WlResource.h"

class ZxdgDecorationManagerV1: public WlResource {
public:
	virtual ~ZxdgDecorationManagerV1() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static ZxdgDecorationManagerV1 *FromResource(struct wl_resource *resource) {
		return (ZxdgDecorationManagerV1*)WlResource::FromResource(resource);
	}

	virtual void HandleDestroy();
	virtual void HandleGetToplevelDecoration(uint32_t id, struct wl_resource *toplevel) = 0;
};

class ZxdgToplevelDecorationV1: public WlResource {
public:
	virtual ~ZxdgToplevelDecorationV1() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static ZxdgToplevelDecorationV1 *FromResource(struct wl_resource *resource) {
		return (ZxdgToplevelDecorationV1*)WlResource::FromResource(resource);
	}

	enum Mode {
		modeClient = 1,
		modeServer = 2,
	};

	virtual void HandleDestroy();
	virtual void HandleSetMode(uint32_t mode) = 0;
	virtual void HandleUnsetMode() = 0;

	void SendConfigure(uint32_t mode) { wl_resource_post_event(ToResource(), 0, mode); }
};
