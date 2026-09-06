/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */


#include "BrightnessWindow.h"

#include <Application.h>
#include <Box.h>
#include <Catalog.h>
#include <GroupLayout.h>
#include <MessageRunner.h>
#include <Screen.h>

#include "BrightnessSlider.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "BrightnessWindow"


BrightnessWindow::BrightnessWindow(BRect frame, bool popUp)
	:
	BWindow(frame, B_TRANSLATE_SYSTEM_NAME("Brightness"),
		popUp ? B_BORDERED_WINDOW_LOOK : B_TITLED_WINDOW_LOOK,
		popUp ? B_FLOATING_ALL_WINDOW_FEEL : B_NORMAL_WINDOW_FEEL,
		B_ASYNCHRONOUS_CONTROLS | B_WILL_ACCEPT_FIRST_CLICK
			| B_AUTO_UPDATE_SIZE_LIMITS | (popUp ? 0 : B_NOT_ZOOMABLE), 0),
	fPopUp(popUp),
	fUpdatedCount(0)
{
	SetLayout(new BGroupLayout(B_HORIZONTAL));

	BGroupLayout* layout = new BGroupLayout(B_HORIZONTAL);
	layout->SetInsets(5, 5, 5, 5);

	BBox* box = new BBox("sliderbox");
	box->SetLayout(layout);
	box->SetBorder(B_PLAIN_BORDER);
	AddChild(box);

	fSlider = new BrightnessSlider();
	fSlider->SetModificationMessage(new BMessage(kMsgBrightnessUpdate));
	box->AddChild(fSlider);

	fSlider->SetTarget(this);
	ResizeTo(300, 50);

	if (!fPopUp) {
		CenterOnScreen();
		return;
	}

	// Make sure the pop-up is not outside the screen.
	const int32 kMargin = 3;
	BRect windowRect = Frame();
	BRect screenFrame(BScreen(B_MAIN_SCREEN_ID).Frame());
	if (screenFrame.right < windowRect.right + kMargin)
		MoveBy(-kMargin - windowRect.right + screenFrame.right, 0);
	if (screenFrame.bottom < windowRect.bottom + kMargin)
		MoveBy(0, -kMargin - windowRect.bottom + screenFrame.bottom);
	if (screenFrame.left > windowRect.left - kMargin)
		MoveBy(kMargin + screenFrame.left - windowRect.left, 0);
	if (screenFrame.top > windowRect.top - kMargin)
		MoveBy(0, kMargin + screenFrame.top - windowRect.top);
}


BrightnessWindow::~BrightnessWindow()
{
}


bool
BrightnessWindow::QuitRequested()
{
	if (!fPopUp)
		be_app->PostMessage(B_QUIT_REQUESTED);

	return true;
}


void
BrightnessWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgBrightnessUpdate:
			fUpdatedCount++;
			break;

		case kMsgBrightnessChanged:
			if (!fPopUp)
				break;

			if (fUpdatedCount < 2) {
				// If the slider was set by a single click, wait a moment
				// before closing, so that the new value can be seen.
				BMessage quit(B_QUIT_REQUESTED);
				BMessageRunner::StartSending(this, &quit, 150000, 1);
			} else
				Quit();
			break;

		case B_QUIT_REQUESTED:
			Quit();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}
