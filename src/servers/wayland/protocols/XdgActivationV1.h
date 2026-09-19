#pragma once
#include "WlResource.h"


class XdgActivationV1: public WlResource {
public:
	virtual ~XdgActivationV1() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgActivationV1 *FromResource(struct wl_resource *resource) {return (XdgActivationV1*)WlResource::FromResource(resource);}

	virtual void HandleDestroy();
	virtual void HandleGetActivationToken(uint32_t id) = 0;
	virtual void HandleActivate(const char *token, struct wl_resource *surface) = 0;
};

class XdgActivationTokenV1: public WlResource {
public:
	virtual ~XdgActivationTokenV1() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static XdgActivationTokenV1 *FromResource(struct wl_resource *resource) {return (XdgActivationTokenV1*)WlResource::FromResource(resource);}

	enum Error {
		errorAlreadyUsed = 0,
	};

	virtual void HandleSetSerial(uint32_t serial, struct wl_resource *seat) = 0;
	virtual void HandleSetAppId(const char *app_id) = 0;
	virtual void HandleSetSurface(struct wl_resource *surface) = 0;
	virtual void HandleCommit() = 0;
	virtual void HandleDestroy();

	void SendDone(const char *token) {wl_resource_post_event(ToResource(), 0, token);}
};
