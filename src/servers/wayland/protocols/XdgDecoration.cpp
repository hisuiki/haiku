#include "XdgDecoration.h"

extern const struct wl_interface zxdg_decoration_manager_v1_interface;

const wl_interface *ZxdgDecorationManagerV1::Interface() const
{
	return &zxdg_decoration_manager_v1_interface;
}

int ZxdgDecorationManagerV1::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetToplevelDecoration(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}

void ZxdgDecorationManagerV1::HandleDestroy()
{
	Destroy();
}

extern const struct wl_interface zxdg_toplevel_decoration_v1_interface;

const wl_interface *ZxdgToplevelDecorationV1::Interface() const
{
	return &zxdg_toplevel_decoration_v1_interface;
}

int ZxdgToplevelDecorationV1::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleSetMode(args[0].u);
			return 0;
		case 2:
			HandleUnsetMode();
			return 0;
	}
	return -1;
}

void ZxdgToplevelDecorationV1::HandleDestroy()
{
	Destroy();
}
