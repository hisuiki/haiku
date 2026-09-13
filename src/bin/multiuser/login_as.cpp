/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <stdio.h>
#include <string.h>

const uint32 kAttemptLogin = 'logi';
const uint32 kLoginOk = 'lok ';

int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Haiku-LoginAs");

	const char* user = argc >= 2 ? argv[1] : "user";
	const char* pass = argc >= 3 ? argv[2] : "";

	BMessenger msgr("application/x-vnd.Haiku-Login");
	if (!msgr.IsValid()) {
		fprintf(stderr, "Login app messenger invalid\n");
		return 1;
	}

	BMessage msg(kAttemptLogin);
	msg.AddString("login", user);
	msg.AddString("password", pass);

	BMessage reply;
	status_t status = msgr.SendMessage(&msg, &reply, 10000000, 10000000);
	if (status != B_OK) {
		fprintf(stderr, "SendMessage failed: %s\n", strerror(status));
		return 1;
	}

	if (reply.what == kLoginOk) {
		printf("Login successful for user %s\n", user);
		return 0;
	}

	int32 err = B_ERROR;
	reply.FindInt32("error", &err);
	printf("Login reply: 0x%" B_PRIx32 ", error: %s\n", reply.what, strerror(err));
	return 0;
}
