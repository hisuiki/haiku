/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */


#include "Brightness.h"

#include <stdio.h>
#include <string.h>

#include <math.h>

#include <Alert.h>
#include <Application.h>
#include <Catalog.h>
#include <Deskbar.h>
#include <Entry.h>
#include <Screen.h>
#include <View.h>

#include "BrightnessWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Brightness"


const char* kSignature = "application/x-vnd.Haiku-Brightness";
const char* kDeskbarItemName = "Brightness";


status_t
our_image(image_info& image)
{
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		if ((char*)our_image >= (char*)image.text
			&& (char*)our_image <= (char*)image.text + image.text_size) {
			return B_OK;
		}
	}

	return B_ERROR;
}


int32
get_screen_brightness()
{
	float brightness;
	if (BScreen(B_MAIN_SCREEN_ID).GetBrightness(&brightness) != B_OK)
		return -1;

	return (int32)roundf(brightness * 100);
}


status_t
set_screen_brightness(int32 percent)
{
	if (percent < 0)
		percent = 0;
	else if (percent > 100)
		percent = 100;

	return BScreen(B_MAIN_SCREEN_ID).SetBrightness(percent / 100.0f);
}


void
draw_brightness_icon(BView* view, BRect rect, float level)
{
	float size = min_c(rect.Width(), rect.Height());
	BPoint center(roundf(rect.left + rect.Width() / 2),
		roundf(rect.top + rect.Height() / 2));

	float radius = floorf(size * 0.22f);
	float rayInner = radius + floorf(size * 0.12f);
	float rayOuter = floorf(size * 0.46f);

	view->PushState();
	view->SetPenSize(max_c(1, floorf(size / 12)));
	view->SetDrawingMode(B_OP_OVER);
	view->StrokeEllipse(center, radius, radius);

	if (level > 0.0f) {
		// Fill the disc from the bottom, so that the icon alone shows how
		// bright the screen is.
		BRect disc(center.x - radius, center.y - radius, center.x + radius,
			center.y + radius);
		disc.top = disc.bottom - ceilf(disc.Height() * min_c(level, 1.0f));

		view->PushState();
		view->ClipToRect(disc);
		view->FillEllipse(center, radius, radius);
		view->PopState();
	}

	for (int32 i = 0; i < 8; i++) {
		double angle = i * M_PI / 4;
		view->StrokeLine(BPoint(center.x + cos(angle) * rayInner,
				center.y + sin(angle) * rayInner),
			BPoint(center.x + cos(angle) * rayOuter,
				center.y + sin(angle) * rayOuter));
	}

	if (level < 0.0f) {
		view->StrokeLine(BPoint(center.x - rayOuter, center.y + rayOuter),
			BPoint(center.x + rayOuter, center.y - rayOuter));
	}

	view->PopState();
}


//	#pragma mark -


class Brightness : public BApplication {
public:
								Brightness();
	virtual						~Brightness();

	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	void				ReadyToRun();

private:
			bool				fAutoInstallInDeskbar;
};


Brightness::Brightness()
	:
	BApplication(kSignature),
	fAutoInstallInDeskbar(false)
{
}


Brightness::~Brightness()
{
}


void
Brightness::ArgvReceived(int32 argc, char** argv)
{
	if (argc <= 1)
		return;

	if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		puts("Brightness options:\n"
			"\t--deskbar\tautomatically add replicant to Deskbar\n"
			"\t--help\t\tprint this info and exit");
		Quit();
		return;
	}

	if (strcmp(argv[1], "--deskbar") == 0)
		fAutoInstallInDeskbar = true;
}


void
Brightness::ReadyToRun()
{
	if (get_screen_brightness() < 0) {
		BString text(B_TRANSLATE("The graphics driver does not support "
			"changing the screen brightness, so %appname% cannot be used on "
			"your system."));
		text.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("Brightness"));

		if (!fAutoInstallInDeskbar) {
			BAlert* alert = new BAlert("", text, B_TRANSLATE("Too bad!"),
				NULL, NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go();
		}

		Quit();
		return;
	}

	bool isInstalled = false;
	bool isDeskbarRunning = true;

	{
		// if the Deskbar is not alive at this point, it might be after having
		// acknowledged the requester below
		BDeskbar deskbar;
		isDeskbarRunning = deskbar.IsRunning();
		isInstalled = deskbar.HasItem(kDeskbarItemName);
	}

	if (isInstalled && fAutoInstallInDeskbar) {
		Quit();
		return;
	}

	if (isDeskbarRunning && !isInstalled) {
		BString text(B_TRANSLATE("You can run %appname% in a window or "
			"install it in the Deskbar."));
		text.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("Brightness"));
		BAlert* alert = new BAlert("", text, B_TRANSLATE("Run in window"),
			B_TRANSLATE("Install in Deskbar"), NULL, B_WIDTH_AS_USUAL,
			B_INFO_ALERT);
		alert->SetShortcut(0, B_ESCAPE);

		if (fAutoInstallInDeskbar || alert->Go() == 1) {
			image_info info;
			entry_ref ref;

			if (our_image(info) == B_OK
				&& get_ref_for_path(info.name, &ref) == B_OK) {
				BDeskbar deskbar;
				deskbar.AddItem(&ref);
			}

			Quit();
			return;
		}
	}

	BWindow* window = new BrightnessWindow(BRect(200, 150, 500, 200), false);
	window->Show();
}


//	#pragma mark -


int
main(int argc, char* argv[])
{
	Brightness app;
	app.Run();

	return 0;
}
