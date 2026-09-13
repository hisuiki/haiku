/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <LaunchRoster.h>
#include <OS.h>
#include <LaunchRosterPrivate.h>


static const char* const kLoginServiceUser = "_login";


int
main(int argc, char** argv)
{
	if (getuid() != 0)
		return EXIT_FAILURE;

	BLaunchRoster roster;

	if (argc >= 2) {
		const char* user = argv[1];
		const char* pass = (argc >= 3) ? argv[2] : "";
		status_t status = BLaunchRoster::Private(roster).SwitchSession(user, pass);
		if (status != B_OK) {
			fprintf(stderr, "autologin: could not switch to session of \"%s\": %s\n",
				user, strerror(status));
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}

	status_t status = BLaunchRoster::Private(roster).StartLoginSession();

	// Only a system that has no login service account at all starts a desktop
	// without asking who is there; the launch daemon reports that, and waits
	// for the user database before it does. Any other failure leaves the
	// machine at the boot screen rather than logging someone in unasked.
	if (status == B_NAME_NOT_FOUND) {
		debug_printf("autologin: no \"%s\" account; starting the desktop of "
			"uid 0, as a single-user system does\n", kLoginServiceUser);
		struct passwd* passwd = getpwuid(0);
		if (passwd == NULL)
			return EXIT_FAILURE;
		status = roster.StartSession(passwd->pw_name);
	} else if (status != B_OK && status != B_ALREADY_RUNNING) {
		debug_printf("autologin: could not start the login session: %s\n",
			strerror(status));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
