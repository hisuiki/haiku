#pragma once
#include "WlResource.h"


class ZwpTextInputV3: public WlResource {
public:
	virtual ~ZwpTextInputV3() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static ZwpTextInputV3 *FromResource(struct wl_resource *resource) {return (ZwpTextInputV3*)WlResource::FromResource(resource);}

	enum ChangeCause {
		changeCauseInputMethod = 0,
		changeCauseOther = 1,
	};

	enum ContentHint {
		contentHintNone = 0x0,
		contentHintCompletion = 0x1,
		contentHintSpellcheck = 0x2,
		contentHintAutoCapitalization = 0x4,
		contentHintLowercase = 0x8,
		contentHintUppercase = 0x10,
		contentHintTitlecase = 0x20,
		contentHintHiddenText = 0x40,
		contentHintSensitiveData = 0x80,
		contentHintLatin = 0x100,
		contentHintMultiline = 0x200,
	};

	enum ContentPurpose {
		contentPurposeNormal = 0,
		contentPurposeAlpha = 1,
		contentPurposeDigits = 2,
		contentPurposeNumber = 3,
		contentPurposePhone = 4,
		contentPurposeUrl = 5,
		contentPurposeEmail = 6,
		contentPurposeName = 7,
		contentPurposePassword = 8,
		contentPurposePin = 9,
		contentPurposeDate = 10,
		contentPurposeTime = 11,
		contentPurposeDatetime = 12,
		contentPurposeTerminal = 13,
	};

	virtual void HandleDestroy();
	virtual void HandleEnable() = 0;
	virtual void HandleDisable() = 0;
	virtual void HandleSetSurroundingText(const char *text, int32_t cursor, int32_t anchor) = 0;
	virtual void HandleSetTextChangeCause(uint32_t cause) = 0;
	virtual void HandleSetContentType(uint32_t hint, uint32_t purpose) = 0;
	virtual void HandleSetCursorRectangle(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	virtual void HandleCommit() = 0;

	void SendEnter(struct wl_resource *surface) {wl_resource_post_event(ToResource(), 0, surface);}
	void SendLeave(struct wl_resource *surface) {wl_resource_post_event(ToResource(), 1, surface);}
	void SendPreeditString(const char *text, int32_t cursor_begin, int32_t cursor_end) {wl_resource_post_event(ToResource(), 2, text, cursor_begin, cursor_end);}
	void SendCommitString(const char *text) {wl_resource_post_event(ToResource(), 3, text);}
	void SendDeleteSurroundingText(uint32_t before_length, uint32_t after_length) {wl_resource_post_event(ToResource(), 4, before_length, after_length);}
	void SendDone(uint32_t serial) {wl_resource_post_event(ToResource(), 5, serial);}
};

class ZwpTextInputManagerV3: public WlResource {
public:
	virtual ~ZwpTextInputManagerV3() {}
	const wl_interface *Interface() const override;
	int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args) override;
	static ZwpTextInputManagerV3 *FromResource(struct wl_resource *resource) {return (ZwpTextInputManagerV3*)WlResource::FromResource(resource);}

	virtual void HandleDestroy();
	virtual void HandleGetTextInput(uint32_t id, struct wl_resource *seat) = 0;
};
