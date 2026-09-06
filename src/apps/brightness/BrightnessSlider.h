/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */
#ifndef BRIGHTNESS_SLIDER_H
#define BRIGHTNESS_SLIDER_H


#include <Slider.h>
#include <String.h>


static const uint32 kMsgBrightnessUpdate = 'brup';
static const uint32 kMsgBrightnessChanged = 'brcg';


/*! The brightness counterpart of desklink's VolumeControl: a slider which
	applies its value to the screen backlight, both as the content of the
	pop-up window of the Deskbar item and as a replicant of its own.
*/
class BrightnessSlider : public BSlider {
public:
								BrightnessSlider();
								BrightnessSlider(BMessage* archive);
	virtual						~BrightnessSlider();

	static	BrightnessSlider*	Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

protected:
	virtual	void				AttachedToWindow();

	virtual	void				MouseDown(BPoint where);

	virtual	void				MessageReceived(BMessage* message);
	virtual	status_t			Invoke(BMessage* message = NULL);

	virtual	void				DrawBar();
	virtual	void				DrawText();

	virtual	const char*			UpdateText() const;

private:
			void				_Init();
			bool				_IsReplicant() const;

	mutable	BString				fText;
			bool				fSupported;
};


#endif	// BRIGHTNESS_SLIDER_H
