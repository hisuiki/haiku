#include "TextInputUnstableV3.h"


extern const struct wl_interface zwp_text_input_v3_interface;

const wl_interface *ZwpTextInputV3::Interface() const
{
	return &zwp_text_input_v3_interface;
}

int ZwpTextInputV3::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleEnable();
			return 0;
		case 2:
			HandleDisable();
			return 0;
		case 3:
			HandleSetSurroundingText(args[0].s, args[1].i, args[2].i);
			return 0;
		case 4:
			HandleSetTextChangeCause(args[0].u);
			return 0;
		case 5:
			HandleSetContentType(args[0].u, args[1].u);
			return 0;
		case 6:
			HandleSetCursorRectangle(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 7:
			HandleCommit();
			return 0;
	}
	return -1;
}

void ZwpTextInputV3::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface zwp_text_input_manager_v3_interface;

const wl_interface *ZwpTextInputManagerV3::Interface() const
{
	return &zwp_text_input_manager_v3_interface;
}

int ZwpTextInputManagerV3::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetTextInput(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}

void ZwpTextInputManagerV3::HandleDestroy()
{
	Destroy();
}
