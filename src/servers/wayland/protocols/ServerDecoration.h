#pragma once
#include "WlResource.h"


class OrgKdeKwinServerDecorationManager: public WlResource {
public:
	virtual ~OrgKdeKwinServerDecorationManager() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static OrgKdeKwinServerDecorationManager *FromResource(struct wl_resource *resource) {return (OrgKdeKwinServerDecorationManager*)WlResource::FromResource(resource);}

	enum Mode {
		modeNone = 0,
		modeClient = 1,
		modeServer = 2,
	};

	virtual void HandleCreate(uint32_t id, struct wl_resource *surface) = 0;

	void SendDefaultMode(uint32_t mode) {wl_resource_post_event(ToResource(), 0, mode);}
};

class OrgKdeKwinServerDecoration: public WlResource {
public:
	virtual ~OrgKdeKwinServerDecoration() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static OrgKdeKwinServerDecoration *FromResource(struct wl_resource *resource) {return (OrgKdeKwinServerDecoration*)WlResource::FromResource(resource);}

	enum Mode {
		modeNone = 0,
		modeClient = 1,
		modeServer = 2,
	};

	virtual void HandleRelease();
	virtual void HandleRequestMode(uint32_t mode) = 0;

	void SendMode(uint32_t mode) {wl_resource_post_event(ToResource(), 0, mode);}
};
