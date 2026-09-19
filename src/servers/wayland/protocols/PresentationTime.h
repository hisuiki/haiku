#pragma once
#include "WlResource.h"


class WpPresentation: public WlResource {
public:
	virtual ~WpPresentation() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WpPresentation *FromResource(struct wl_resource *resource) {return (WpPresentation*)WlResource::FromResource(resource);}

	enum Error {
		errorInvalidTimestamp = 0,
		errorInvalidFlag = 1,
	};

	virtual void HandleDestroy();
	virtual void HandleFeedback(struct wl_resource *surface, uint32_t callback) = 0;

	void SendClockId(uint32_t clk_id) {wl_resource_post_event(ToResource(), 0, clk_id);}
};

class WpPresentationFeedback: public WlResource {
public:
	virtual ~WpPresentationFeedback() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static WpPresentationFeedback *FromResource(struct wl_resource *resource) {return (WpPresentationFeedback*)WlResource::FromResource(resource);}

	enum Kind {
		kindVsync = 0x1,
		kindHwClock = 0x2,
		kindHwCompletion = 0x4,
		kindZeroCopy = 0x8,
	};

	void SendSyncOutput(struct wl_resource *output) {wl_resource_post_event(ToResource(), 0, output);}
	void SendPresented(uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {wl_resource_post_event(ToResource(), 1, tv_sec_hi, tv_sec_lo, tv_nsec, refresh, seq_hi, seq_lo, flags);}
	void SendDiscarded() {wl_resource_post_event(ToResource(), 2);}
};
