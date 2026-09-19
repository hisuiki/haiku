#include "Viewporter.h"


extern const struct wl_interface wp_viewporter_interface;

const wl_interface *WpViewporter::Interface() const
{
	return &wp_viewporter_interface;
}

int WpViewporter::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetViewport(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}

void WpViewporter::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wp_viewport_interface;

const wl_interface *WpViewport::Interface() const
{
	return &wp_viewport_interface;
}

int WpViewport::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleSetSource(args[0].f, args[1].f, args[2].f, args[3].f);
			return 0;
		case 2:
			HandleSetDestination(args[0].i, args[1].i);
			return 0;
	}
	return -1;
}

void WpViewport::HandleDestroy()
{
	Destroy();
}
