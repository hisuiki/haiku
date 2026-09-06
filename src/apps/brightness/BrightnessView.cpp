/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */


#include "BrightnessView.h"

#include <math.h>
#include <stdio.h>

#include <Alert.h>
#include <Application.h>
#include <Catalog.h>
#include <Deskbar.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <String.h>

#include "Brightness.h"
#include "BrightnessWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "BrightnessView"


static const uint32 kMsgOpenScreenSettings = 'oscs';
static const uint32 kMsgRemoveFromDeskbar = 'rmdb';

static const int32 kWheelStep = 5;

extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth,
	float maxHeight);


BrightnessView::BrightnessView(BRect frame, uint32 resizingMode,
	bool inDeskbar)
	:
	BView(frame, kDeskbarItemName, resizingMode,
		B_WILL_DRAW | B_TRANSPARENT_BACKGROUND | B_FRAME_EVENTS),
	fInDeskbar(inDeskbar)
{
}


BrightnessView::BrightnessView(BMessage* archive)
	:
	BView(archive),
	fInDeskbar(true)
{
}


BrightnessView::~BrightnessView()
{
}


BrightnessView*
BrightnessView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "BrightnessView"))
		return NULL;

	return new BrightnessView(archive);
}


status_t
BrightnessView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", kSignature);
	if (status == B_OK)
		status = archive->AddString("class", "BrightnessView");

	return status;
}


void
BrightnessView::AttachedToWindow()
{
	BView::AttachedToWindow();

	AdoptParentColors();
	SetDrawingMode(B_OP_ALPHA);
	SetFlags(Flags() | B_SUBPIXEL_PRECISE);

	_UpdateToolTip();
}


/*!	Draws the sun which stands for the brightness, filled in proportion to the
	current level so that the item alone already shows how bright the screen
	is.
*/
void
BrightnessView::Draw(BRect updateRect)
{
	int32 brightness = get_screen_brightness();
	draw_brightness_icon(this, Bounds(),
		brightness < 0 ? -1.0f : brightness / 100.0f);
}


void
BrightnessView::MouseDown(BPoint where)
{
	if (Looper() == NULL || Looper()->CurrentMessage() == NULL)
		return;

	uint32 buttons;
	if (Looper()->CurrentMessage()->FindInt32("buttons", (int32*)&buttons)
			!= B_OK) {
		buttons = 0;
	}

	BPoint whereScreen;
	if (Looper()->CurrentMessage()->FindPoint("screen_where", &whereScreen)
			!= B_OK) {
		whereScreen = ConvertToScreen(where);
	}

	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
		BPopUpMenu* menu = new BPopUpMenu("", false, false);
		menu->SetFont(be_plain_font);

		menu->AddItem(new BMenuItem(
			B_TRANSLATE("Screen preferences" B_UTF8_ELLIPSIS),
			new BMessage(kMsgOpenScreenSettings)));

		if (fInDeskbar) {
			menu->AddSeparatorItem();
			menu->AddItem(new BMenuItem(B_TRANSLATE("Remove from Deskbar"),
				new BMessage(kMsgRemoveFromDeskbar)));
		}

		menu->SetTargetForItems(this);
		BRect menuFrame(whereScreen - BPoint(4, 4), whereScreen + BPoint(4, 4));
		menu->Go(whereScreen, true, false, menuFrame, true);
	} else {
		BRect windowFrame(whereScreen, BSize(207, 19));
		BrightnessWindow* window = new BrightnessWindow(windowFrame);
		window->Show();
	}

	BView::MouseDown(where);
}


void
BrightnessView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOpenScreenSettings:
			_OpenScreenPreferences();
			break;

		case kMsgRemoveFromDeskbar:
		{
			BDeskbar deskbar;
			deskbar.RemoveItem(kDeskbarItemName);
			break;
		}

		case B_MOUSE_WHEEL_CHANGED:
		{
			float deltaY;
			if (message->FindFloat("be:wheel_delta_y", &deltaY) == B_OK
				&& deltaY != 0) {
				_ChangeBrightnessBy(deltaY < 0 ? kWheelStep : -kWheelStep);
			}
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
BrightnessView::_ChangeBrightnessBy(int32 delta)
{
	int32 brightness = get_screen_brightness();
	if (brightness < 0)
		return;

	if (set_screen_brightness(brightness + delta) != B_OK)
		return;

	_UpdateToolTip();
	Invalidate();
}


void
BrightnessView::_UpdateToolTip()
{
	int32 brightness = get_screen_brightness();
	if (brightness < 0) {
		SetToolTip(B_TRANSLATE("Brightness control unavailable"));
		return;
	}

	BString text;
	text.SetToFormat(B_TRANSLATE("Brightness: %" B_PRId32 "%%"), brightness);
	SetToolTip(text.String());
}


void
BrightnessView::_OpenScreenPreferences()
{
	const char* kScreenSignature = "application/x-vnd.Haiku-Screen";
	if (be_roster->Launch(kScreenSignature) == B_OK
		|| be_roster->Launch(kScreenSignature) == B_ALREADY_RUNNING) {
		return;
	}

	BPath path;
	if (find_directory(B_SYSTEM_PREFERENCES_DIRECTORY, &path) != B_OK)
		return;

	path.Append("Screen");

	entry_ref ref;
	if (get_ref_for_path(path.Path(), &ref) == B_OK)
		be_roster->Launch(&ref);
}


//	#pragma mark -


extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	return new BrightnessView(BRect(0, 0, maxHeight - 1, maxHeight - 1),
		B_FOLLOW_LEFT | B_FOLLOW_TOP, true);
}
