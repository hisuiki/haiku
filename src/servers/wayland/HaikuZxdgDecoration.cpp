#include "HaikuZxdgDecoration.h"
#include "HaikuXdgToplevel.h"
#include <AutoDeleter.h>
#include <stdio.h>

extern const struct wl_interface zxdg_decoration_manager_v1_interface;

enum {
	ZXDG_DECORATION_VERSION = 1,
};

class HaikuZxdgDecorationManager: public ZxdgDecorationManagerV1 {
protected:
	virtual ~HaikuZxdgDecorationManager() = default;

public:
	void HandleGetToplevelDecoration(uint32_t id, struct wl_resource *toplevel_resource) final;
};

HaikuZxdgDecorationManagerGlobal *HaikuZxdgDecorationManagerGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuZxdgDecorationManagerGlobal> global(
		new(std::nothrow) HaikuZxdgDecorationManagerGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zxdg_decoration_manager_v1_interface, ZXDG_DECORATION_VERSION))
		return NULL;
	return global.Detach();
}

void HaikuZxdgDecorationManagerGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuZxdgDecorationManager *manager = new(std::nothrow) HaikuZxdgDecorationManager();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!manager->Init(wl_client, version, id))
		return;
}

void HaikuZxdgDecorationManager::HandleGetToplevelDecoration(uint32_t id, struct wl_resource *toplevel_resource)
{
	HaikuXdgToplevel *toplevel = HaikuXdgToplevel::FromResource(toplevel_resource);
	if (toplevel == NULL)
		return;
	HaikuZxdgToplevelDecoration::Create(this, toplevel, id);
}

HaikuZxdgToplevelDecoration *HaikuZxdgToplevelDecoration::Create(
	HaikuZxdgDecorationManager *manager, HaikuXdgToplevel *toplevel, uint32_t id)
{
	HaikuZxdgToplevelDecoration *decor = new(std::nothrow) HaikuZxdgToplevelDecoration();
	if (!decor) {
		wl_client_post_no_memory(manager->Client());
		return NULL;
	}
	if (!decor->Init(manager->Client(), manager->Version(), id))
		return NULL;

	decor->fToplevel = toplevel;
	decor->fMode = modeServer;
	toplevel->SetZxdgDecoration(decor);

	// Immediately configure server-side mode:
	decor->SendConfigure(modeServer);
	return decor;
}

HaikuZxdgToplevelDecoration::~HaikuZxdgToplevelDecoration()
{
	if (fToplevel != NULL) {
		fToplevel->SetZxdgDecoration(NULL);
		fToplevel = NULL;
	}
}

void HaikuZxdgToplevelDecoration::HandleSetMode(uint32_t mode)
{
	fMode = modeServer;
	if (fToplevel != NULL && fToplevel->Window() != NULL)
		fToplevel->Window()->SetLook(B_TITLED_WINDOW_LOOK);
	SendConfigure(modeServer);
}

void HaikuZxdgToplevelDecoration::HandleUnsetMode()
{
	fMode = modeServer;
	if (fToplevel != NULL && fToplevel->Window() != NULL)
		fToplevel->Window()->SetLook(B_TITLED_WINDOW_LOOK);
	SendConfigure(modeServer);
}
