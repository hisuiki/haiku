#include "XdgShell.h"


extern const struct wl_interface xdg_wm_base_interface;

const wl_interface *XdgWmBase::Interface() const
{
	return &xdg_wm_base_interface;
}

int XdgWmBase::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleCreatePositioner(args[0].n);
			return 0;
		case 2:
			HandleGetXdgSurface(args[0].n, (wl_resource*)args[1].o);
			return 0;
		case 3:
			HandlePong(args[0].u);
			return 0;
	}
	return -1;
}

void XdgWmBase::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface xdg_positioner_interface;

const wl_interface *XdgPositioner::Interface() const
{
	return &xdg_positioner_interface;
}

int XdgPositioner::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleSetSize(args[0].i, args[1].i);
			return 0;
		case 2:
			HandleSetAnchorRect(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 3:
			HandleSetAnchor(args[0].u);
			return 0;
		case 4:
			HandleSetGravity(args[0].u);
			return 0;
		case 5:
			HandleSetConstraintAdjustment(args[0].u);
			return 0;
		case 6:
			HandleSetOffset(args[0].i, args[1].i);
			return 0;
		case 7:
			HandleSetReactive();
			return 0;
		case 8:
			HandleSetParentSize(args[0].i, args[1].i);
			return 0;
		case 9:
			HandleSetParentConfigure(args[0].u);
			return 0;
	}
	return -1;
}

void XdgPositioner::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface xdg_surface_interface;

const wl_interface *XdgSurface::Interface() const
{
	return &xdg_surface_interface;
}

int XdgSurface::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetToplevel(args[0].n);
			return 0;
		case 2:
			HandleGetPopup(args[0].n, (wl_resource*)args[1].o, (wl_resource*)args[2].o);
			return 0;
		case 3:
			HandleSetWindowGeometry(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 4:
			HandleAckConfigure(args[0].u);
			return 0;
	}
	return -1;
}

void XdgSurface::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface xdg_toplevel_interface;

const wl_interface *XdgToplevel::Interface() const
{
	return &xdg_toplevel_interface;
}

int XdgToplevel::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleSetParent((wl_resource*)args[0].o);
			return 0;
		case 2:
			HandleSetTitle(args[0].s);
			return 0;
		case 3:
			HandleSetAppId(args[0].s);
			return 0;
		case 4:
			HandleShowWindowMenu((wl_resource*)args[0].o, args[1].u, args[2].i, args[3].i);
			return 0;
		case 5:
			HandleMove((wl_resource*)args[0].o, args[1].u);
			return 0;
		case 6:
			HandleResize((wl_resource*)args[0].o, args[1].u, args[2].u);
			return 0;
		case 7:
			HandleSetMaxSize(args[0].i, args[1].i);
			return 0;
		case 8:
			HandleSetMinSize(args[0].i, args[1].i);
			return 0;
		case 9:
			HandleSetMaximized();
			return 0;
		case 10:
			HandleUnsetMaximized();
			return 0;
		case 11:
			HandleSetFullscreen((wl_resource*)args[0].o);
			return 0;
		case 12:
			HandleUnsetFullscreen();
			return 0;
		case 13:
			HandleSetMinimized();
			return 0;
	}
	return -1;
}

void XdgToplevel::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface xdg_popup_interface;

const wl_interface *XdgPopup::Interface() const
{
	return &xdg_popup_interface;
}

int XdgPopup::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGrab((wl_resource*)args[0].o, args[1].u);
			return 0;
		case 2:
			HandleReposition((wl_resource*)args[0].o, args[1].u);
			return 0;
	}
	return -1;
}

void XdgPopup::HandleDestroy()
{
	Destroy();
}
