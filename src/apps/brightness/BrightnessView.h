/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */
#ifndef BRIGHTNESS_VIEW_H
#define BRIGHTNESS_VIEW_H


#include <View.h>


/*! The Deskbar item: a sun which opens the brightness slider when clicked,
	and which reacts to the mouse wheel.
*/
class BrightnessView : public BView {
public:
								BrightnessView(BRect frame,
									uint32 resizingMode, bool inDeskbar);
								BrightnessView(BMessage* archive);
	virtual						~BrightnessView();

	static	BrightnessView*		Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_ChangeBrightnessBy(int32 delta);
			void				_UpdateToolTip();
			void				_OpenScreenPreferences();

			bool				fInDeskbar;
};


#endif	// BRIGHTNESS_VIEW_H
