#pragma once
#include "WlResource.h"


class WpViewporter: public WlResource {
public:
	virtual ~WpViewporter() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WpViewporter *FromResource(struct wl_resource *resource) {return (WpViewporter*)WlResource::FromResource(resource);}

	enum Error {
		errorViewportExists = 0,
	};

	virtual void HandleDestroy();
	virtual void HandleGetViewport(uint32_t id, struct wl_resource *surface) = 0;
};

class WpViewport: public WlResource {
public:
	virtual ~WpViewport() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WpViewport *FromResource(struct wl_resource *resource) {return (WpViewport*)WlResource::FromResource(resource);}

	enum Error {
		errorBadValue = 0,
		errorBadSize = 1,
		errorOutOfBuffer = 2,
		errorNoSurface = 3,
	};

	virtual void HandleDestroy();
	virtual void HandleSetSource(wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) = 0;
	virtual void HandleSetDestination(int32_t width, int32_t height) = 0;
};
