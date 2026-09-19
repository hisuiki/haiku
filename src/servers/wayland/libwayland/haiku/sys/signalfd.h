/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WAYLAND_HAIKU_SYS_SIGNALFD_H
#define _WAYLAND_HAIKU_SYS_SIGNALFD_H


#include <fcntl.h>
#include <signal.h>
#include <stdint.h>


#define SFD_CLOEXEC		O_CLOEXEC
#define SFD_NONBLOCK	O_NONBLOCK


struct signalfd_siginfo {
	uint32_t	ssi_signo;
	uint8_t		_reserved[124];
};


#define signalfd	wl_haiku_signalfd


#ifdef __cplusplus
extern "C" {
#endif

int signalfd(int fd, const sigset_t* mask, int flags);

#ifdef __cplusplus
}
#endif


#endif	/* _WAYLAND_HAIKU_SYS_SIGNALFD_H */
