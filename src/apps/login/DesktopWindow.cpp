/*
 * Copyright 2008, François Revol, <revol@free.fr>. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <string.h>
#include <stdio.h>

#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Screen.h>
#include <View.h>
#include <WindowPrivate.h>

#include "LoginApp.h"
#include "DesktopWindow.h"

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Desktop Window"


DesktopWindow::DesktopWindow(BRect frame)
	: BWindow(frame, B_TRANSLATE("Desktop"),
		kDesktopWindowLook,
		kDesktopWindowFeel,
		B_NOT_MOVABLE | B_NOT_CLOSABLE | B_NOT_ZOOMABLE
		 | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE
		 | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES)
{
	BScreen screen;
	BView *desktop = new BView(Bounds(), "desktop", B_FOLLOW_NONE, 0);
	desktop->SetViewColor(screen.DesktopColor());
	AddChild(desktop);

	// load the shelf
	BPath path;
	status_t err;
	entry_ref ref;
	err = find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &path, true);
	if (err >= B_OK) {
		BDirectory dir(path.Path());
		if (!dir.Contains("x-vnd.Haiku-Login", B_DIRECTORY_NODE))
			dir.CreateDirectory("x-vnd.Haiku-Login", NULL);
		path.Append("x-vnd.Haiku-Login");
		dir.SetTo(path.Path());
		if (!dir.Contains("Shelf", B_FILE_NODE))
			dir.CreateFile("Shelf", NULL);
		path.Append("Shelf");
		get_ref_for_path(path.Path(), &ref);
	}

	// The desktop behind the login window is not editable: what it shows will
	// be set in a preferences panel of its own.
	fDesktopShelf = new BShelf(&ref, desktop, false, "DesktopShelf");
	if (fDesktopShelf)
		fDesktopShelf->SetDisplaysZombies(true);
}


DesktopWindow::~DesktopWindow()
{
	delete fDesktopShelf;
}


bool
DesktopWindow::QuitRequested()
{
	status_t err;
	err = fDesktopShelf->Save();
	printf(B_TRANSLATE_COMMENT("error %s\n",
		"A return message from fDesktopShelf->Save(). It can be \"B_OK\""),
		strerror(err));
	return BWindow::QuitRequested();
}


void
DesktopWindow::DispatchMessage(BMessage *message, BHandler *handler)
{
	switch (message->what) {
		case B_MOUSE_DOWN:
		case B_MOUSE_UP:
		case B_MOUSE_MOVED:
		case B_KEY_DOWN:
		case B_KEY_UP:
		case B_UNMAPPED_KEY_DOWN:
		case B_UNMAPPED_KEY_UP:
			/* don't allow interacting with the replicants */
			break;
		default:
		BWindow::DispatchMessage(message, handler);
	}
}


