#pragma once
#include "Wayland.h"
#include "WlGlobal.h"
#include "ServerDecoration.h"

#include <Window.h>


class HaikuSurface;
class HaikuServerDecorationManager;

class HaikuServerDecorationManagerGlobal: public WlGlocal {
public:
	static HaikuServerDecorationManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuServerDecorationManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

class HaikuServerDecoration: public OrgKdeKwinServerDecoration {
private:
	friend class HaikuSurface;
	HaikuSurface *fSurface;
	OrgKdeKwinServerDecoration::Mode fMode = OrgKdeKwinServerDecoration::modeServer;

public:
	static HaikuServerDecoration *Create(HaikuServerDecorationManager *manager, HaikuSurface *surface, uint32_t id);
	static HaikuServerDecoration *FromResource(struct wl_resource *resource) {return (HaikuServerDecoration*)WlResource::FromResource(resource);}
	virtual ~HaikuServerDecoration();

	inline OrgKdeKwinServerDecoration::Mode Mode() {return fMode;}
	window_look Look();

	void HandleRequestMode(uint32_t mode) final;
};
