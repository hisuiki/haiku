#include "PresentationTime.h"


extern const struct wl_interface wp_presentation_interface;

const wl_interface *WpPresentation::Interface() const
{
	return &wp_presentation_interface;
}

int WpPresentation::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleFeedback((wl_resource*)args[0].o, args[1].n);
			return 0;
	}
	return -1;
}

void WpPresentation::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wp_presentation_feedback_interface;

const wl_interface *WpPresentationFeedback::Interface() const
{
	return &wp_presentation_feedback_interface;
}

int WpPresentationFeedback::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	return -1;
}
