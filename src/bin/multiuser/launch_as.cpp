/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//! Runs a program under another account.


#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "multiuser_utils.h"


extern const char* __progname;


static void
print_usage_and_exit(bool error)
{
	fprintf(error ? stderr : stdout,
		"Usage: %s <user> <program> [ <argument> ... ]\n"
		"Runs the program as the given user. Only the superuser may do so;\n"
		"it is how the launch daemon starts the services that have an account\n"
		"of their own.\n", __progname);
	exit(error ? 1 : 0);
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && (strcmp(argv[1], "--help") == 0
			|| strcmp(argv[1], "-h") == 0)) {
		print_usage_and_exit(false);
	}
	if (argc < 3)
		print_usage_and_exit(true);

	if (geteuid() != 0) {
		fprintf(stderr, "%s: only the superuser can run a program as another "
			"user.\n", __progname);
		return 1;
	}

	const char* name = argv[1];
	struct passwd* passwd = getpwnam(name);
	if (passwd == NULL) {
		fprintf(stderr, "%s: there is no \"%s\" account.\n", __progname, name);
		return 1;
	}

	// A service account's settings live in its home, which an upgraded system
	// has never created.
	struct stat st;
	if (passwd->pw_dir != NULL && passwd->pw_dir[0] == '/'
		&& stat(passwd->pw_dir, &st) != 0) {
		status_t status = create_user_home(passwd->pw_dir, passwd->pw_uid,
			passwd->pw_gid, false);
		if (status != B_OK) {
			fprintf(stderr, "%s: could not create the home \"%s\" of \"%s\": "
				"%s\n", __progname, passwd->pw_dir, name, strerror(status));
		}
	}

	// The groups first: dropping the user ID first would take away the right
	// to change them.
	if (initgroups(name, passwd->pw_gid) != 0
		|| setgid(passwd->pw_gid) != 0
		|| setuid(passwd->pw_uid) != 0) {
		fprintf(stderr, "%s: could not become \"%s\": %s\n", __progname, name,
			strerror(errno));
		return 1;
	}

	execv(argv[2], argv + 2);

	fprintf(stderr, "%s: could not run \"%s\": %s\n", __progname, argv[2],
		strerror(errno));
	return 1;
}
