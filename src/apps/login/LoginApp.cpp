/*
 * Copyright 2008, François Revol, <revol@free.fr>. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Alert.h>
#include <Catalog.h>
#include <Screen.h>
#include <String.h>
#include <View.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <LaunchRoster.h>
#include <LaunchRosterPrivate.h>
#include <RosterPrivate.h>

#include "LoginApp.h"
#include "LoginWindow.h"
#include "DesktopWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Login App"

const char *kLoginAppSig = "application/x-vnd.Haiku-Login";


LoginApp::LoginApp()
	:
	BApplication(kLoginAppSig),
	fModalMode(true),
	fQuitAllowed(false)
{
}


LoginApp::~LoginApp()
{
}


void
LoginApp::ReadyToRun()
{
	{
		const char* loginSession = getenv("HAIKU_LOGIN_SESSION");
		if (loginSession == NULL || strcmp(loginSession, "1") != 0) {
			BAlert* alert = new BAlert(B_TRANSLATE("Login unavailable"),
				B_TRANSLATE("Login can only run in the system login session."),
				B_TRANSLATE("OK"));
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go(NULL);
			fQuitAllowed = true;
			PostMessage(B_QUIT_REQUESTED);
			return;
		}
	}

	BScreen screen;

	// The desktop goes up first: whichever window is shown last becomes the
	// active one, and that has to be the login window, or typing goes nowhere
	// until someone clicks into it.
	fDesktopWindow = new DesktopWindow(screen.Frame());
	fDesktopWindow->Show();

	{
		float sizeDelta = (float)be_plain_font->Size()/12.0f;
		BRect frame(0, 0, 450 * sizeDelta, 150 * sizeDelta);
		frame.OffsetBySelf(screen.Frame().Width()/2 - frame.Width()/2,
			screen.Frame().Height()/2 - frame.Height()/2);
		fLoginWindow = new LoginWindow(frame);
		fLoginWindow->Show();
		fLoginWindow->Activate();
	}

	// TODO: add a shelf with Activity Monitor replicant :)
}


void
LoginApp::MessageReceived(BMessage *message)
{
	bool reboot = false;

	switch (message->what) {
		case kAttemptLogin:
			TryLogin(message);
			// TODO
			break;
		case kRebootAction:
			reboot = true;
			// FALLTHROUGH
		case kHaltAction:
		{
			BRoster roster;
			BRoster::Private rosterPrivate(roster);
			status_t error = rosterPrivate.ShutDown(reboot, false, false);
			if (error < B_OK) {
				BString msg(B_TRANSLATE("Error: %1"));
				msg.ReplaceFirst("%1", strerror(error));
				BAlert* alert = new BAlert(("Error"), msg.String(),
					B_TRANSLATE("OK"));
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go();
			}
			break;
		}
		case kSuspendAction:
		{
			BAlert* alert = new BAlert(B_TRANSLATE("Error"),
				B_TRANSLATE("Unimplemented"), B_TRANSLATE("OK"));
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go();
			break;
		}

		default:
			BApplication::MessageReceived(message);
	}
}


void
LoginApp::ArgvReceived(int32 argc, char **argv)
{
	const char* loginSession = getenv("HAIKU_LOGIN_SESSION");
	if (loginSession != NULL && strcmp(loginSession, "1") == 0) {
		if (argc > 1)
			fprintf(stderr, "Login: command line options are disabled in the login session\n");
		return;
	}

	for (int i = 1; i < argc; i++) {
		BString arg(argv[i]);
		//printf("[%d]: %s\n", i, argv[i]);
		if (arg == "--nonmodal" && geteuid() == 0)
			fModalMode = false;
		else /*if (arg == "--help")*/ {
			puts(B_TRANSLATE("Login application for Haiku\nUsage:"));
			printf("%s [--nonmodal]\n", argv[0]);
			puts(B_TRANSLATE("--nonmodal	Do not make the window modal"));
			// just return to the shell
			exit((arg == "--help") ? 0 : 1);
			return;
		}
	}
}


bool
LoginApp::QuitRequested()
{
	if (fQuitAllowed)
		return true;

	// The greeter stays up until someone logs in, but it never stands in the
	// way of shutting down: the registrar calls a shutdown off as soon as any
	// application refuses to quit, which would make the Halt and Reboot
	// buttons, and the power button, do nothing at all.
	BRoster roster;
	bool shuttingDown = false;
	return BRoster::Private(roster).IsShutDownInProgress(&shuttingDown) == B_OK
		&& shuttingDown;
}


void
LoginApp::TryLogin(BMessage *message)
{
	BMessage reply(kLoginBad);
	status_t status = B_BAD_VALUE;

	const char* login;
	if (message->FindString("login", &login) == B_OK) {
		const char* password = message->GetString("password");

		BLaunchRoster roster;
		status = BLaunchRoster::Private(roster).SwitchSession(login, password);
		// On success the greeter stays where it is, behind the session that
		// now has the display. Locking or switching users brings it back.
		if (status != B_OK)
			fprintf(stderr, "SwitchSession: %s\n", strerror(status));
	}

	if (status == B_OK) {
		reply.what = kLoginOk;
		message->SendReply(&reply);
	} else {
		reply.AddInt32("error", status);
		message->SendReply(&reply);
	}
}


int
LoginApp::getpty(char *pty, char *tty)
{
	static const char major[] = "pqrs";
	static const char minor[] = "0123456789abcdef";
	uint32 i, j;
	int32 fd = -1;

	for (i = 0; i < sizeof(major); i++)
	{
		for (j = 0; j < sizeof(minor); j++)
		{
			sprintf(pty, "/dev/pt/%c%c", major[i], minor[j]);
			sprintf(tty, "/dev/tt/%c%c", major[i], minor[j]);
			fd = open(pty, O_RDWR|O_NOCTTY);
			if (fd >= 0)
			{
				return fd;
			}
		}
	}

	return fd;
}
