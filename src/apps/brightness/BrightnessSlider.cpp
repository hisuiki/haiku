/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */


#include "BrightnessSlider.h"

#include <stdio.h>

#include <Catalog.h>
#include <ControlLook.h>
#include <Dragger.h>
#include <MenuItem.h>
#include <Mime.h>
#include <PopUpMenu.h>
#include <Screen.h>

#include <AppMisc.h>
#include <SystemCatalog.h>
#include <ViewPrivate.h>

#include "Brightness.h"
#include "BrightnessWindow.h"


using BPrivate::gSystemCatalog;


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "BrightnessSlider"


static const int32 kWheelStep = 5;


BrightnessSlider::BrightnessSlider()
	:
	BSlider("BrightnessSlider", B_TRANSLATE("Brightness"),
		new BMessage(kMsgBrightnessChanged), 0, 100, B_HORIZONTAL),
	fSupported(false)
{
	_Init();

	font_height fontHeight;
	GetFontHeight(&fontHeight);
	SetBarThickness(ceilf((fontHeight.ascent + fontHeight.descent) * 0.7));

	BRect rect(Bounds());
	rect.top = rect.bottom - 7;
	rect.left = rect.right - 7;
	BDragger* dragger = new BDragger(rect, this,
		B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);

	const char* remove = gSystemCatalog.GetString(
		B_TRANSLATE("Remove replicant"), "Dragger");

	BPopUpMenu* popUp = new BPopUpMenu("brightness", false, false,
		B_ITEMS_IN_COLUMN);
	popUp->AddItem(new BMenuItem(remove, new BMessage(kDeleteReplicant)));
	dragger->SetPopUp(popUp);

	AddChild(dragger);
}


BrightnessSlider::BrightnessSlider(BMessage* archive)
	:
	BSlider(archive),
	fSupported(false)
{
	_Init();

	BMessage message(B_QUIT_REQUESTED);
	archive->SendReply(&message);
}


BrightnessSlider::~BrightnessSlider()
{
}


BrightnessSlider*
BrightnessSlider::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "BrightnessSlider"))
		return NULL;

	return new BrightnessSlider(archive);
}


status_t
BrightnessSlider::Archive(BMessage* into, bool deep) const
{
	status_t status = BView::Archive(into, deep);
	if (status != B_OK)
		return status;

	return into->AddString("add_on", kSignature);
}


void
BrightnessSlider::AttachedToWindow()
{
	BSlider::AttachedToWindow();

	if (_IsReplicant()) {
		SetEventMask(0, 0);
		SetDrawingMode(B_OP_ALPHA);
		SetFlags(Flags() | B_TRANSPARENT_BACKGROUND);
		SetViewColor(B_TRANSPARENT_COLOR);
	} else
		SetEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);

	// The screen may have changed since the slider was archived.
	int32 brightness = get_screen_brightness();
	fSupported = brightness >= 0;
	if (fSupported)
		SetValue(brightness);
	else
		SetLabel(B_TRANSLATE("Brightness control unavailable"));

	SetEnabled(fSupported);
}


/*!	Since we have set a mouse event mask, we don't want to forward all mouse
	downs to the slider: a click next to the bar only invokes us, which closes
	the pop-up window.
*/
void
BrightnessSlider::MouseDown(BPoint where)
{
	if (Looper() == NULL || Looper()->CurrentMessage() == NULL)
		return;

	int32 viewToken;
	if (Looper()->CurrentMessage()->FindInt32("_view_token", &viewToken)
			!= B_OK) {
		viewToken = -1;
	}

	// ignore clicks on the dragger
	if (Bounds().Contains(where) && viewToken >= 0
		&& viewToken != _get_object_token_(this)) {
		return;
	}

	if (!IsEnabled() || !Bounds().Contains(where)) {
		Invoke();
		return;
	}

	BSlider::MouseDown(where);
}


void
BrightnessSlider::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_MOUSE_WHEEL_CHANGED:
		{
			if (!IsEnabled())
				return;

			float deltaY = 0.0f;
			message->FindFloat("be:wheel_delta_y", &deltaY);
			if (deltaY == 0.0f)
				return;

			int32 value = Value() - (int32)deltaY * kWheelStep;
			if (value != Value()) {
				SetValue(value);
				InvokeNotify(ModificationMessage(), B_CONTROL_MODIFIED);
				set_screen_brightness(Value());
			}
			break;
		}

		case B_QUIT_REQUESTED:
			Window()->MessageReceived(message);
			break;

		case B_WORKSPACE_ACTIVATED:
			if (_IsReplicant())
				Invalidate();
			break;

		default:
			BSlider::MessageReceived(message);
			break;
	}
}


status_t
BrightnessSlider::Invoke(BMessage* message)
{
	set_screen_brightness(Value());

	return BSlider::Invoke(message);
}


void
BrightnessSlider::DrawBar()
{
	BRect frame = BarFrame();
	BView* view = OffscreenView();

	// A warm fill, so that the bar reads as brightness rather than as a level.
	rgb_color fillColor = make_color(255, 203, 88, 255);

	be_control_look->DrawSliderBar(view, frame, frame, LowColor(), fillColor,
		be_control_look->Flags(this), Orientation());
}


void
BrightnessSlider::DrawText()
{
	BRect bounds(Bounds());
	BView* view = OffscreenView();
	rgb_color base = view->LowColor();
	rgb_color textColor = view->HighColor();
	uint32 flags = be_control_look->Flags(this);

	font_height fontHeight;
	GetFontHeight(&fontHeight);

	float iconSize = BControlLook::ComposeIconSize(B_MINI_ICON).Width();
	float iconWidth = iconSize + be_control_look->DefaultLabelSpacing();

	view->SetHighColor(textColor);
	draw_brightness_icon(view, BRect(0, 0, iconSize, iconSize),
		IsEnabled() ? Value() / 100.0f : -1.0f);

	if (Label() != NULL) {
		BPoint labelLoc(iconWidth, ceilf(fontHeight.ascent));
		be_control_look->DrawLabel(view, Label(), base, flags, labelLoc,
			&textColor);
	}

	if (fText.Length() > 0) {
		BPoint valueLoc(bounds.right - StringWidth(fText),
			ceilf(fontHeight.ascent));
		be_control_look->DrawLabel(view, fText, base, flags, valueLoc,
			&textColor);
	}
}


const char*
BrightnessSlider::UpdateText() const
{
	if (!IsEnabled())
		return NULL;

	fText.SetToFormat(B_TRANSLATE("%" B_PRId32 "%%"), Value());
	return fText.String();
}


void
BrightnessSlider::_Init()
{
	int32 brightness = get_screen_brightness();
	fSupported = brightness >= 0;

	SetLimits(0, 100);
	SetHashMarks(B_HASH_MARKS_NONE);
	SetValue(fSupported ? brightness : 0);
}


bool
BrightnessSlider::_IsReplicant() const
{
	return dynamic_cast<BrightnessWindow*>(Window()) == NULL;
}
