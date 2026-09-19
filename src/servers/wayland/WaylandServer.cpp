/*
 * Copyright 2022-2025, X512
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "WaylandServer.h"

#include "HaikuCompositor.h"
#include "HaikuDataDeviceManager.h"
#include "HaikuOutput.h"
#include "HaikuSeat.h"
#include "HaikuServerDecoration.h"
#include "HaikuShm.h"
#include "HaikuSubcompositor.h"
#include "HaikuTextInput.h"
#include "HaikuViewporter.h"
#include "HaikuXdgShell.h"
#include "HaikuZxdgDecoration.h"
#include "WaylandEnv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include <new>

#include <Application.h>
#include <AutoDeleter.h>
#include <Looper.h>
#include <String.h>

#include <wayland-server-core.h>
#include <wayland-haiku.h>


ServerHandler gServerHandler;
BMessenger gServerMessenger;

static struct wl_display* sDisplay;


struct Listener {
	Listener*				next;
	BString					path;
	BString					label;
	int						fd;
	struct wl_event_source*	source;
};

static Listener* sListeners;


ServerHandler::ServerHandler()
	:
	BHandler("server")
{
}


void
ServerHandler::MessageReceived(BMessage* message)
{
	BHandler::MessageReceived(message);
}


void
WaylandServerWake()
{
	if (sDisplay != NULL) {
		wl_haiku_event_loop_wake(
			wl_event_loop_get_fd(wl_display_get_event_loop(sDisplay)));
	}
}


static void
log_to_syslog(const char* format, va_list arguments)
{
	vsyslog(LOG_ERR, format, arguments);
}


static int
accept_client(int fd, uint32_t mask, void* data)
{
	int clientFD = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (clientFD < 0)
		return 0;

	if (wl_client_create(sDisplay, clientFD) == NULL) {
		syslog(LOG_ERR, "wayland_server: could not create a client: %s\n",
			strerror(errno));
		close(clientFD);
	}
	return 0;
}


static void
remove_listener(Listener* listener)
{
	for (Listener** link = &sListeners; *link != NULL;
			link = &(*link)->next) {
		if (*link == listener) {
			*link = listener->next;
			break;
		}
	}

	wl_event_source_remove(listener->source);
	close(listener->fd);
	unlink(listener->path.String());
	delete listener;
}


static status_t
add_listener(const char* path, const char* label)
{
	if (path == NULL || path[0] != '/')
		return B_BAD_VALUE;

	struct sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	if (strlcpy(address.sun_path, path, sizeof(address.sun_path))
			>= sizeof(address.sun_path)) {
		return B_NAME_TOO_LONG;
	}

	for (Listener* listener = sListeners; listener != NULL;
			listener = listener->next) {
		if (listener->path == path) {
			remove_listener(listener);
			break;
		}
	}

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return errno;
	FileDescriptorCloser fdCloser(fd);

	unlink(path);
	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0
		|| listen(fd, 128) != 0) {
		return errno;
	}

	// The clients are Linux programs running as the container's account,
	// which is not the account this service runs as. Who may reach the
	// socket is decided by where it is, not by its mode.
	chmod(path, 0666);

	Listener* listener = new(std::nothrow) Listener;
	if (listener == NULL) {
		unlink(path);
		return B_NO_MEMORY;
	}
	listener->path = path;
	listener->label = label != NULL ? label : "";
	listener->fd = fd;
	listener->source = wl_event_loop_add_fd(wl_display_get_event_loop(sDisplay),
		fd, WL_EVENT_READABLE, accept_client, listener);
	if (listener->source == NULL) {
		unlink(path);
		delete listener;
		return B_NO_MEMORY;
	}

	fdCloser.Detach();
	listener->next = sListeners;
	sListeners = listener;
	return B_OK;
}


static status_t
remove_listener_at(const char* path)
{
	if (path == NULL)
		return B_BAD_VALUE;

	for (Listener* listener = sListeners; listener != NULL;
			listener = listener->next) {
		if (listener->path == path) {
			remove_listener(listener);
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


class Application : public BApplication {
public:
								Application();

	virtual	void				MessageReceived(BMessage* message);
};


Application::Application()
	:
	BApplication(kWaylandServerSignature)
{
}


void
Application::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_KEY_MAP_LOADED:
		{
			WaylandEnv env(this);
			HaikuSeatGlobal* seat = HaikuGetSeat(NULL);
			if (seat != NULL)
				seat->UpdateKeymap();
			return;
		}

		case kMsgAddSocket:
		case kMsgRemoveSocket:
		{
			const char* path = NULL;
			message->FindString("path", &path);
			const char* label = NULL;
			message->FindString("label", &label);

			status_t status;
			{
				WaylandEnv env(this);
				status = message->what == kMsgAddSocket
					? add_listener(path, label) : remove_listener_at(path);
			}
			if (status != B_OK) {
				syslog(LOG_ERR, "wayland_server: %s %s: %s\n",
					message->what == kMsgAddSocket ? "cannot listen at"
						: "cannot stop listening at",
					path != NULL ? path : "(none)", strerror(status));
			}

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			return;
		}
	}

	BApplication::MessageReceived(message);
}


static void
unlock_server(void*)
{
	wl_display_flush_clients(sDisplay);
	gServerHandler.UnlockLooper();
}


static void
lock_server(void*)
{
	gServerHandler.LockLooper();
}


static status_t
dispatch_thread(void*)
{
	gServerHandler.LockLooper();
	struct wl_event_loop* loop = wl_display_get_event_loop(sDisplay);
	for (;;) {
		wl_event_loop_dispatch(loop, -1);
		wl_display_flush_clients(sDisplay);
	}
	return B_OK;
}


static bool
create_globals(struct wl_display* display)
{
	HaikuSeatGlobal* seat = NULL;
	return HaikuShmGlobal::Create(display) != NULL
		&& HaikuCompositorGlobal::Create(display) != NULL
		&& HaikuSubcompositorGlobal::Create(display) != NULL
		&& HaikuViewporterGlobal::Create(display) != NULL
		&& HaikuOutputGlobal::Create(display) != NULL
		&& HaikuDataDeviceManagerGlobal::Create(display) != NULL
		&& (seat = HaikuSeatGlobal::Create(display)) != NULL
		&& HaikuTextInputGlobal::Create(display, seat) != NULL
		&& HaikuXdgShell::Create(display) != NULL
		&& HaikuServerDecorationManagerGlobal::Create(display) != NULL
		&& HaikuZxdgDecorationManagerGlobal::Create(display) != NULL;
}


int
main(int argc, char** argv)
{
	// Started on demand by whoever first needs a display, this would otherwise
	// hold on to that program's terminal for as long as it runs.
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		if (null > STDERR_FILENO)
			close(null);
	}

	Application* application = new Application;

	BLooper* looper = new BLooper("wayland", B_DISPLAY_PRIORITY);
	looper->AddHandler(&gServerHandler);
	gServerMessenger.SetTo(&gServerHandler);
	looper->Run();

	gServerHandler.LockLooper();
	wl_log_set_handler_server(log_to_syslog);
	sDisplay = wl_display_create();
	if (sDisplay == NULL || !create_globals(sDisplay)) {
		syslog(LOG_ERR, "wayland_server: could not set up the display\n");
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		status_t status = add_listener(argv[i], NULL);
		if (status != B_OK) {
			syslog(LOG_ERR, "wayland_server: cannot listen at %s: %s\n",
				argv[i], strerror(status));
		}
	}
	gServerHandler.UnlockLooper();

	wl_haiku_event_loop_set_wait_hooks(unlock_server, lock_server, NULL);

	thread_id thread = spawn_thread(dispatch_thread, "wayland dispatch",
		B_DISPLAY_PRIORITY, NULL);
	if (thread < 0 || resume_thread(thread) != B_OK)
		return 1;

	application->Run();
	delete application;
	return 0;
}
