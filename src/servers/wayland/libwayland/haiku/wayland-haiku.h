/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WAYLAND_HAIKU_H
#define _WAYLAND_HAIKU_H


#ifdef __cplusplus
extern "C" {
#endif

/* Called around every blocking wait of the event loop, so that the thread
 * running it can give up a lock that other threads need while it sleeps. */
void wl_haiku_event_loop_set_wait_hooks(void (*before)(void* data),
	void (*after)(void* data), void* data);

/* Makes a blocked wl_event_loop_dispatch() on the loop owning \a epollFD
 * return. Safe to call from any thread. */
void wl_haiku_event_loop_wake(int epollFD);

#ifdef __cplusplus
}
#endif


#endif	/* _WAYLAND_HAIKU_H */
