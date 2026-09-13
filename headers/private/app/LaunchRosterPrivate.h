/*
 * Copyright 2015 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LAUNCH_ROSTER_PRIVATE_H
#define _LAUNCH_ROSTER_PRIVATE_H


#include <LaunchRoster.h>


class BLaunchRoster::Private {
public:
								Private(BLaunchRoster* roster);
								Private(BLaunchRoster& roster);

			status_t			RegisterSessionDaemon(const BMessenger& daemon);
			status_t			StartLoginSession();
			status_t			SwitchSession(const char* login,
									const char* password);
			status_t			LogoutSession();
			status_t			LockSession();
			status_t			LaunchInDisplaySession(const char* program);
			status_t			GetDisplaySessionUser(uid_t& _user);

private:
			BLaunchRoster*		fRoster;
};


#endif	// _LAUNCH_ROSTER_PRIVATE_H
