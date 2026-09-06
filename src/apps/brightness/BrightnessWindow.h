/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */
#ifndef BRIGHTNESS_WINDOW_H
#define BRIGHTNESS_WINDOW_H


#include <Window.h>


class BrightnessSlider;


/*! The pop-up which appears when the Deskbar item is clicked, and the window
	of the application when it is started on its own.
*/
class BrightnessWindow : public BWindow {
public:
								BrightnessWindow(BRect frame,
									bool popUp = true);
	virtual						~BrightnessWindow();

			BrightnessSlider*	Slider() const { return fSlider; }

protected:
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			BrightnessSlider*	fSlider;
			bool				fPopUp;
			int32				fUpdatedCount;
};


#endif	// BRIGHTNESS_WINDOW_H
