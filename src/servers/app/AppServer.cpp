/*
 * Copyright 2001-2016, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Axel Dörfler, axeld@pinc-software.de
 *		Stephan Aßmus <superstippi@gmx.de>
 * 		Christian Packmann
 */


#include "AppServer.h"

#include <unistd.h>

#include <OS.h>

#include <syslog.h>

#include <AutoDeleter.h>
#include <LaunchRoster.h>
#include <PortLink.h>
#include <RosterPrivate.h>

#include "BitmapManager.h"
#include "Desktop.h"
#include "GlobalFontManager.h"
#include "InputManager.h"
#include "ScreenManager.h"
#include "ServerProtocol.h"


//#define DEBUG_SERVER
#ifdef DEBUG_SERVER
#	include <stdio.h>
#	define STRACE(x) printf x
#else
#	define STRACE(x) ;
#endif


// Globals
port_id gAppServerPort;
BTokenSpace gTokenSpace;
uint32 gAppServerSIMDFlags = 0;


/*!	\brief Constructor

	This loads the default fonts, allocates all the major global variables,
	spawns the main housekeeping threads, loads user preferences for the UI
	and decorator, and allocates various locks.
*/
AppServer::AppServer(status_t* status)
	:
	SERVER_BASE("application/x-vnd.Haiku-app_server", "picasso", -1, false,
		status),
	fActiveDesktop(NULL),
	fDesktopLock("AppServerDesktopLock")
{
	openlog("app_server", 0, LOG_DAEMON);

	gInputManager = new InputManager();

	// Create the font server and scan the proper directories.
	gFontManager = new GlobalFontManager;
	if (gFontManager->InitCheck() != B_OK)
		debugger("font manager could not be initialized!");

	gFontManager->Run();

	gScreenManager = new ScreenManager();
	gScreenManager->Run();

	// Create the bitmap allocator. Object declared in BitmapManager.cpp
	gBitmapManager = new BitmapManager();

#ifndef HAIKU_TARGET_PLATFORM_LIBBE_TEST
#if 0
	// This is not presently needed, as app_server is launched from the login session.
	// TODO: check the attached displays, and launch login session for them
	BMessage data;
	data.AddString("name", "app_server");
	data.AddInt32("session", 0);
	BLaunchRoster().Target("login", data);
#endif

	// Inform the registrar we've (re)started.
	BMessage request(kMsgAppServerStarted);
	BRoster::Private().SendTo(&request, NULL, false);
#endif
}


/*!	\brief Destructor
	Reached only when the server is asked to shut down in Test mode.
*/
AppServer::~AppServer()
{
	delete gBitmapManager;

	gScreenManager->Lock();
	gScreenManager->Quit();

	gFontManager->Lock();
	gFontManager->Quit();

	closelog();
}


void
AppServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case AS_GET_DESKTOP:
		{
			Desktop* desktop = NULL;

			// Which desktop a client belongs to follows from who it runs as,
			// and that is what the kernel says about its team - never the user
			// ID in the request, which the client fills in itself.
			team_id callerTeam = message->ReturnAddress().Team();
			team_info callerInfo;
			if (callerTeam < 0
				|| get_team_info(callerTeam, &callerInfo) != B_OK) {
				BMessage reply((uint32)B_PERMISSION_DENIED);
				message->SendReply(&reply);
				break;
			}

			int32 userID = callerInfo.uid;
			int32 version = message->GetInt32("version", 0);
			const char* targetScreen = message->GetString("target");

			if (version != AS_PROTOCOL_VERSION) {
				syslog(LOG_ERR, "Application for user %" B_PRId32 " does not "
					"support the current server protocol (%" B_PRId32 ").\n",
					userID, version);
			} else {
				// A seat is a login session, and every client of one runs in
				// it: the kernel says which, whatever the client runs as. The
				// system services started outside any session - the
				// input_server, the media services - share the session of the
				// launch daemon, and attach to whichever seat is in front
				// rather than having one of their own.
				desktop = _FindDesktopForSession(callerInfo.session_id);
				if (desktop == NULL) {
					// Not a seat: a system service, or a program run over a
					// network login, which is a session of its own to the
					// kernel but not one anybody sits at. Those attach to
					// whichever seat is in front. Seats themselves are made
					// when the launch daemon says a login session has started.
					desktop = fActiveDesktop;
				}

				if (desktop == NULL) {
					// Nothing has a seat yet, and something needs to draw.
					desktop = _CreateDesktop(userID, targetScreen,
						callerInfo.session_id);
				}
			}

			BMessage reply;
			if (desktop != NULL)
				reply.AddInt32("port", desktop->MessagePort());
			else
				reply.what = (uint32)B_ERROR;

			message->SendReply(&reply);
			break;
		}

		case AS_ACTIVATE_DESKTOP:
		{
			// Only the launch daemon decides which session is in front, and it
			// runs as root; the kernel says who is really asking.
			team_id callerTeam = message->ReturnAddress().Team();
			team_info callerInfo;
			status_t status = callerTeam >= 0
				&& get_team_info(callerTeam, &callerInfo) == B_OK
					? B_OK : B_PERMISSION_DENIED;
			if (status == B_OK && callerInfo.uid != 0)
				status = B_PERMISSION_DENIED;

			int32 userID = message->GetInt32("user", -1);
			if (status == B_OK && userID < 0)
				status = B_BAD_VALUE;
			if (status == B_OK) {
				status = _ActivateDesktop((uid_t)userID,
					message->GetBool("create", false),
					(pid_t)message->GetInt32("session", 0));
			}

			BMessage reply((uint32)status);
			message->SendReply(&reply);
			break;
		}

		case AS_CLOSE_DESKTOP:
		{
			team_id callerTeam = message->ReturnAddress().Team();
			team_info callerInfo;
			status_t status = callerTeam >= 0
				&& get_team_info(callerTeam, &callerInfo) == B_OK
					? B_OK : B_PERMISSION_DENIED;
			if (status == B_OK && callerInfo.uid != 0)
				status = B_PERMISSION_DENIED;

			if (status == B_OK) {
				status = _CloseDesktop(
					(pid_t)message->GetInt32("session", 0));
			}

			BMessage reply((uint32)status);
			message->SendReply(&reply);
			break;
		}

		default:
			// We don't allow application scripting
			STRACE(("AppServer received unexpected code %" B_PRId32 "\n",
				message->what));
			break;
	}
}


bool
AppServer::QuitRequested()
{
#if TEST_MODE
	while (fDesktops.CountItems() > 0) {
		Desktop *desktop = fDesktops.RemoveItemAt(0);

		thread_id thread = desktop->Thread();
		desktop->PostMessage(B_QUIT_REQUESTED);

		// we just wait for the desktop to kill itself
		status_t status;
		wait_for_thread(thread, &status);
	}

	delete this;
	exit(0);

	return SERVER_BASE::QuitRequested();
#else
	return false;
#endif

}


/*!	\brief Creates a desktop object for an authorized user
*/
Desktop*
AppServer::_CreateDesktop(uid_t userID, const char* targetScreen, pid_t sessionID)
{
	BAutolock locker(fDesktopLock);

	// A desktop takes the display as it starts, so whoever has it must give it
	// up first. The session that has just started is the one in front.
	if (fActiveDesktop != NULL)
		fActiveDesktop->SuspendScreen();

	ObjectDeleter<Desktop> desktop;
	try {
		desktop.SetTo(new Desktop(userID, targetScreen));
		desktop->SetSessionID(sessionID);

		status_t status = desktop->Init();
		if (status == B_OK)
			status = desktop->Run();
		if (status == B_OK && !fDesktops.AddItem(desktop.Get()))
			status = B_NO_MEMORY;

		if (status != B_OK) {
			syslog(LOG_ERR, "Cannot initialize Desktop object: %s\n",
				strerror(status));
			return NULL;
		}
	} catch (...) {
		// there is obviously no memory left
		return NULL;
	}

	fActiveDesktop = desktop.Get();
	syslog(LOG_INFO, "made a desktop for user %d (session %d)\n",
		(int)userID, (int)sessionID);

	return desktop.Detach();
}


/*!	Gives the display to the desktop of \a userID, and takes it from whichever
	desktop has it: this is what switching between the sessions of different
	users comes down to. The sessions themselves keep running either way.
*/
Desktop*
AppServer::_FindDesktopForSession(pid_t sessionID)
{
	if (sessionID <= 0)
		return NULL;

	BAutolock locker(fDesktopLock);

	for (int32 i = 0; i < fDesktops.CountItems(); i++) {
		Desktop* desktop = fDesktops.ItemAt(i);
		if (desktop->SessionID() == sessionID)
			return desktop;
	}

	return NULL;
}


/*!	Drops the desktop of a session that has ended, freeing the screen it was
	suspended to. The desktop in front is never closed: whoever ends a session
	gives the display to another one first.
*/
status_t
AppServer::_CloseDesktop(pid_t sessionID)
{
	BAutolock locker(fDesktopLock);

	Desktop* desktop = _FindDesktopForSession(sessionID);
	if (desktop == NULL)
		return B_NAME_NOT_FOUND;
	if (desktop == fActiveDesktop) {
		desktop->SuspendScreen();
		fActiveDesktop = NULL;
	}

	fDesktops.RemoveItem(desktop, false);
	locker.Unlock();

	desktop->PostMessage(B_QUIT_REQUESTED);
	return B_OK;
}


status_t
AppServer::_ActivateDesktop(uid_t userID, bool createIfNeeded, pid_t sessionID)
{
	BAutolock locker(fDesktopLock);

	// The desktop of this session, if it has one already.
	Desktop* desktop = _FindDesktopForSession(sessionID);
	if (desktop == NULL) {
		desktop = _FindDesktop(userID, NULL);
		if (desktop != NULL && sessionID > 0) {
			if (desktop->SessionID() == 0) {
				// The first desktop the app_server made, before any session
				// existed, for the services that were already running. The
				// session of the same user takes it over.
				desktop->SetSessionID(sessionID);
			} else if (desktop->SessionID() != sessionID) {
				// It belongs to another session of the same user.
				desktop = NULL;
			}
		}
	}

	if (desktop == NULL && createIfNeeded) {
		// A session has just started: it gets a desktop of its own, and that
		// desktop comes to the front.
		desktop = _CreateDesktop(userID, NULL, sessionID);
		if (desktop == NULL)
			return B_ERROR;

		return B_OK;
	}
	if (desktop == NULL)
		return B_NAME_NOT_FOUND;
	if (desktop == fActiveDesktop && !desktop->IsScreenSuspended())
		return B_OK;

	if (fActiveDesktop != NULL && fActiveDesktop != desktop) {
		status_t status = fActiveDesktop->SuspendScreen();
		if (status != B_OK)
			return status;
	}

	status_t status = desktop->ResumeScreen();
	syslog(LOG_INFO, "desktop of user %d takes the screen: %s\n",
		(int)desktop->UserID(), strerror(status));
	if (status != B_OK)
		return status;

	fActiveDesktop = desktop;
	return B_OK;
}


void
AppServer::InputServerRegistered()
{
	BAutolock locker(fDesktopLock);

	if (fActiveDesktop != NULL && !fActiveDesktop->IsScreenSuspended())
		fActiveDesktop->TakeInput();
}


/*!	\brief Finds the desktop object that belongs to a certain user
*/
Desktop*
AppServer::_FindDesktop(uid_t userID, const char* targetScreen)
{
	BAutolock locker(fDesktopLock);

	for (int32 i = 0; i < fDesktops.CountItems(); i++) {
		Desktop* desktop = fDesktops.ItemAt(i);

		if (desktop->UserID() == userID
			&& ((desktop->TargetScreen() == NULL && targetScreen == NULL)
				|| (desktop->TargetScreen() != NULL && targetScreen != NULL
					&& strcmp(desktop->TargetScreen(), targetScreen) == 0))) {
			return desktop;
		}
	}

	return NULL;
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	srand(real_time_clock_usecs());

	status_t status;
	AppServer* server = new AppServer(&status);
	if (status == B_OK)
		server->Run();

	return status == B_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
