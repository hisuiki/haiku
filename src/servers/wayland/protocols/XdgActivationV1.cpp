#include "XdgActivationV1.h"


extern const struct wl_interface xdg_activation_v1_interface;

const wl_interface *XdgActivationV1::Interface() const
{
	return &xdg_activation_v1_interface;
}

int XdgActivationV1::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetActivationToken(args[0].n);
			return 0;
		case 2:
			HandleActivate(args[0].s, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}

void XdgActivationV1::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface xdg_activation_token_v1_interface;

const wl_interface *XdgActivationTokenV1::Interface() const
{
	return &xdg_activation_token_v1_interface;
}

int XdgActivationTokenV1::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleSetSerial(args[0].u, (wl_resource*)args[1].o);
			return 0;
		case 1:
			HandleSetAppId(args[0].s);
			return 0;
		case 2:
			HandleSetSurface((wl_resource*)args[0].o);
			return 0;
		case 3:
			HandleCommit();
			return 0;
		case 4:
			HandleDestroy();
			return 0;
	}
	return -1;
}

void XdgActivationTokenV1::HandleDestroy()
{
	Destroy();
}
