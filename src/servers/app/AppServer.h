/*
 * Copyright 2001-2015, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Axel Dörfler, axeld@pinc-software.de
 */
#ifndef	APP_SERVER_H
#define	APP_SERVER_H


#include <Application.h>
#include <List.h>
#include <Locker.h>
#include <ObjectList.h>
#include <OS.h>
#include <String.h>
#include <Window.h>

#include "MessageLooper.h"
#include "ServerConfig.h"


#ifndef HAIKU_TARGET_PLATFORM_LIBBE_TEST
#	include <Server.h>
#	define SERVER_BASE BServer
#else
#	include "TestServerLoopAdapter.h"
#	define SERVER_BASE TestServerLoopAdapter
#endif


class ServerApp;
class BitmapManager;
class Desktop;


class AppServer : public SERVER_BASE {
public:
								AppServer(status_t* status);
	virtual						~AppServer();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

			void				InputServerRegistered();

private:
			Desktop*			_CreateDesktop(uid_t userID,
									const char* targetScreen,
									pid_t sessionID = 0);
			status_t			_ActivateDesktop(uid_t userID,
									bool createIfNeeded,
									pid_t sessionID = 0);
	virtual	Desktop*			_FindDesktop(uid_t userID,
									const char* targetScreen);
			Desktop*			_FindDesktopForSession(pid_t sessionID);
			status_t			_CloseDesktop(pid_t sessionID);

			void				_LaunchInputServer();

private:
			BObjectList<Desktop> fDesktops;
			Desktop*			fActiveDesktop;
			BLocker				fDesktopLock;
};


extern BitmapManager *gBitmapManager;
extern port_id gAppServerPort;


#endif	/* APP_SERVER_H */
