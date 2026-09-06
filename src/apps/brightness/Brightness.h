/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */
#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H


#include <image.h>
#include <Rect.h>
#include <SupportDefs.h>


class BView;


extern const char* kSignature;
extern const char* kDeskbarItemName;

status_t our_image(image_info& image);

/*! Returns the brightness of the main screen in percent, or a negative error
	code if the graphics driver does not support brightness control.
*/
int32 get_screen_brightness();

/*! Sets the brightness of the main screen, in percent. */
status_t set_screen_brightness(int32 percent);

/*! Draws the sun which stands for the brightness into \a rect, with its disc
	filled from the bottom in proportion to \a level (0 to 1). A negative
	level draws the sun struck through, for when brightness cannot be
	controlled at all.
*/
void draw_brightness_icon(BView* view, BRect rect, float level);


#endif	// BRIGHTNESS_H
