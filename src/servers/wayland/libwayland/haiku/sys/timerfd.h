/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WAYLAND_HAIKU_SYS_TIMERFD_H
#define _WAYLAND_HAIKU_SYS_TIMERFD_H


#include <fcntl.h>
#include <time.h>


#define TFD_CLOEXEC			O_CLOEXEC
#define TFD_NONBLOCK		O_NONBLOCK
#define TFD_TIMER_ABSTIME	1


#define timerfd_create	wl_haiku_timerfd_create
#define timerfd_settime	wl_haiku_timerfd_settime


#ifdef __cplusplus
extern "C" {
#endif

int timerfd_create(clockid_t clock, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec* value,
	struct itimerspec* oldValue);

#ifdef __cplusplus
}
#endif


#endif	/* _WAYLAND_HAIKU_SYS_TIMERFD_H */
