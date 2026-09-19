#pragma once
#include "Wayland.h"
#include "WlGlobal.h"
#include "XdgDecoration.h"
#include <Window.h>

class HaikuXdgToplevel;
class HaikuZxdgDecorationManager;

class HaikuZxdgDecorationManagerGlobal: public WlGlocal {
public:
	static HaikuZxdgDecorationManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuZxdgDecorationManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

class HaikuZxdgToplevelDecoration: public ZxdgToplevelDecorationV1 {
private:
	friend class HaikuXdgToplevel;
	HaikuXdgToplevel *fToplevel{};
	uint32_t fMode = modeServer;

public:
	static HaikuZxdgToplevelDecoration *Create(HaikuZxdgDecorationManager *manager,
		HaikuXdgToplevel *toplevel, uint32_t id);
	static HaikuZxdgToplevelDecoration *FromResource(struct wl_resource *resource) {
		return (HaikuZxdgToplevelDecoration*)WlResource::FromResource(resource);
	}
	virtual ~HaikuZxdgToplevelDecoration();

	uint32_t Mode() const { return fMode; }
	window_look Look() const {
		return B_TITLED_WINDOW_LOOK;
	}

	void HandleSetMode(uint32_t mode) final;
	void HandleUnsetMode() final;
};
