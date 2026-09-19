/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WAYLAND_HAIKU_SYS_EPOLL_H
#define _WAYLAND_HAIKU_SYS_EPOLL_H


#include <stdint.h>
#include <fcntl.h>


#define EPOLLIN			0x001
#define EPOLLPRI		0x002
#define EPOLLOUT		0x004
#define EPOLLERR		0x008
#define EPOLLHUP		0x010
#define EPOLLRDHUP		0x2000

#define EPOLL_CLOEXEC	O_CLOEXEC

#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3


typedef union epoll_data {
	void*		ptr;
	int			fd;
	uint32_t	u32;
	uint64_t	u64;
} epoll_data_t;

struct epoll_event {
	uint32_t		events;
	epoll_data_t	data;
};


#define epoll_create	wl_haiku_epoll_create
#define epoll_create1	wl_haiku_epoll_create1
#define epoll_ctl		wl_haiku_epoll_ctl
#define epoll_wait		wl_haiku_epoll_wait


#ifdef __cplusplus
extern "C" {
#endif

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxEvents,
	int timeout);

#ifdef __cplusplus
}
#endif


#endif	/* _WAYLAND_HAIKU_SYS_EPOLL_H */
