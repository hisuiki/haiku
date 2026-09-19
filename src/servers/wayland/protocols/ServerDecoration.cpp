#include "ServerDecoration.h"


extern const struct wl_interface org_kde_kwin_server_decoration_manager_interface;

const wl_interface *OrgKdeKwinServerDecorationManager::Interface() const
{
	return &org_kde_kwin_server_decoration_manager_interface;
}

int OrgKdeKwinServerDecorationManager::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleCreate(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}


extern const struct wl_interface org_kde_kwin_server_decoration_interface;

const wl_interface *OrgKdeKwinServerDecoration::Interface() const
{
	return &org_kde_kwin_server_decoration_interface;
}

int OrgKdeKwinServerDecoration::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleRelease();
			return 0;
		case 1:
			HandleRequestMode(args[0].u);
			return 0;
	}
	return -1;
}

void OrgKdeKwinServerDecoration::HandleRelease()
{
	Destroy();
}
