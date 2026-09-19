/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The epoll, timerfd and signalfd calls libwayland's event loop is written
 * against, implemented over poll() for this one user. Only what the loop does
 * is covered: level-triggered interest sets, one-shot absolute timers.
 */


#include "sys/epoll.h"
#include "sys/signalfd.h"
#include "sys/timerfd.h"
#include "wayland-haiku.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


struct registration {
	int			fd;
	uint32_t	events;
	epoll_data_t data;
};

struct epoll_set {
	struct epoll_set*		next;
	int						fd;
	int						wakeFD;
	struct registration*	registrations;
	int						count;
	int						capacity;
};

struct timer {
	struct timer*	next;
	int				fd;
	bool			armed;
	struct timespec	deadline;
};


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static struct epoll_set* sSets;
static struct timer* sTimers;

static void (*sBeforeWait)(void*);
static void (*sAfterWait)(void*);
static void* sWaitData;


static struct epoll_set*
find_set(int fd)
{
	for (struct epoll_set* set = sSets; set != NULL; set = set->next) {
		if (set->fd == fd)
			return set;
	}
	return NULL;
}


static struct timer*
find_timer(int fd)
{
	for (struct timer* timer = sTimers; timer != NULL; timer = timer->next) {
		if (timer->fd == fd)
			return timer;
	}
	return NULL;
}


static void
forget_fd(int fd)
{
	// A descriptor number is reused as soon as it is closed, and nothing
	// tells this code when that happens; a stale entry is dropped as soon as
	// the number turns up in a new role.
	struct epoll_set** setLink = &sSets;
	while (*setLink != NULL) {
		struct epoll_set* set = *setLink;
		if (set->fd == fd) {
			*setLink = set->next;
			close(set->wakeFD);
			free(set->registrations);
			free(set);
		} else
			setLink = &set->next;
	}

	struct timer** timerLink = &sTimers;
	while (*timerLink != NULL) {
		struct timer* timer = *timerLink;
		if (timer->fd == fd) {
			*timerLink = timer->next;
			free(timer);
		} else
			timerLink = &timer->next;
	}
}


static int64_t
nanoseconds(const struct timespec* time)
{
	return (int64_t)time->tv_sec * 1000000000LL + time->tv_nsec;
}


int
epoll_create1(int flags)
{
	int pipes[2];
	if (pipe(pipes) != 0)
		return -1;

	fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	fcntl(pipes[1], F_SETFL, O_NONBLOCK);
	fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
	if ((flags & EPOLL_CLOEXEC) != 0)
		fcntl(pipes[0], F_SETFD, FD_CLOEXEC);

	struct epoll_set* set = calloc(1, sizeof(struct epoll_set));
	if (set == NULL) {
		close(pipes[0]);
		close(pipes[1]);
		errno = ENOMEM;
		return -1;
	}
	set->fd = pipes[0];
	set->wakeFD = pipes[1];

	pthread_mutex_lock(&sLock);
	forget_fd(set->fd);
	set->next = sSets;
	sSets = set;
	pthread_mutex_unlock(&sLock);
	return set->fd;
}


int
epoll_create(int size)
{
	return epoll_create1(0);
}


int
epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)
{
	pthread_mutex_lock(&sLock);
	struct epoll_set* set = find_set(epfd);
	if (set == NULL) {
		pthread_mutex_unlock(&sLock);
		errno = EBADF;
		return -1;
	}

	int index = -1;
	for (int i = 0; i < set->count; i++) {
		if (set->registrations[i].fd == fd) {
			index = i;
			break;
		}
	}

	int error = 0;
	switch (op) {
		case EPOLL_CTL_ADD:
			if (index >= 0) {
				error = EEXIST;
				break;
			}
			if (set->count == set->capacity) {
				int capacity = set->capacity == 0 ? 16 : set->capacity * 2;
				struct registration* registrations = realloc(
					set->registrations, capacity * sizeof(struct registration));
				if (registrations == NULL) {
					error = ENOMEM;
					break;
				}
				set->registrations = registrations;
				set->capacity = capacity;
			}
			set->registrations[set->count].fd = fd;
			set->registrations[set->count].events = event->events;
			set->registrations[set->count].data = event->data;
			set->count++;
			break;

		case EPOLL_CTL_MOD:
			if (index < 0) {
				error = ENOENT;
				break;
			}
			set->registrations[index].events = event->events;
			set->registrations[index].data = event->data;
			break;

		case EPOLL_CTL_DEL:
			if (index < 0) {
				error = ENOENT;
				break;
			}
			set->registrations[index] = set->registrations[--set->count];
			break;

		default:
			error = EINVAL;
			break;
	}
	pthread_mutex_unlock(&sLock);

	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}


int
epoll_wait(int epfd, struct epoll_event* events, int maxEvents, int timeout)
{
	if (maxEvents <= 0) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&sLock);
	struct epoll_set* set = find_set(epfd);
	if (set == NULL) {
		pthread_mutex_unlock(&sLock);
		errno = EBADF;
		return -1;
	}

	int count = set->count;
	struct pollfd* pollFDs = malloc((count + 1) * sizeof(struct pollfd));
	struct registration* snapshot = malloc(
		(count > 0 ? count : 1) * sizeof(struct registration));
	if (pollFDs == NULL || snapshot == NULL) {
		pthread_mutex_unlock(&sLock);
		free(pollFDs);
		free(snapshot);
		errno = ENOMEM;
		return -1;
	}
	memcpy(snapshot, set->registrations, count * sizeof(struct registration));

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int64_t waitNanoseconds = timeout < 0 ? -1 : (int64_t)timeout * 1000000;
	for (int i = 0; i < count; i++) {
		pollFDs[i].fd = snapshot[i].fd;
		pollFDs[i].events = 0;
		pollFDs[i].revents = 0;

		struct timer* timer = find_timer(snapshot[i].fd);
		if (timer != NULL) {
			// A timer is never polled: when it is due is known here.
			pollFDs[i].fd = -1;
			if (timer->armed && (snapshot[i].events & EPOLLIN) != 0) {
				int64_t remaining = nanoseconds(&timer->deadline)
					- nanoseconds(&now);
				if (remaining < 0)
					remaining = 0;
				if (waitNanoseconds < 0 || remaining < waitNanoseconds)
					waitNanoseconds = remaining;
			}
			continue;
		}

		if ((snapshot[i].events & EPOLLIN) != 0)
			pollFDs[i].events |= POLLIN;
		if ((snapshot[i].events & EPOLLOUT) != 0)
			pollFDs[i].events |= POLLOUT;
	}
	pollFDs[count].fd = set->fd;
	pollFDs[count].events = POLLIN;
	pollFDs[count].revents = 0;
	pthread_mutex_unlock(&sLock);

	int pollTimeout = waitNanoseconds < 0 ? -1
		: (int)((waitNanoseconds + 999999) / 1000000);

	bool hooked = pollTimeout != 0 && sBeforeWait != NULL;
	if (hooked)
		sBeforeWait(sWaitData);
	int result = poll(pollFDs, count + 1, pollTimeout);
	int pollError = errno;
	if (hooked)
		sAfterWait(sWaitData);

	if (result < 0 && pollError != EINTR) {
		free(pollFDs);
		free(snapshot);
		errno = pollError;
		return -1;
	}

	if (pollFDs[count].revents != 0) {
		char buffer[64];
		while (read(pollFDs[count].fd, buffer, sizeof(buffer)) > 0)
			;
	}

	pthread_mutex_lock(&sLock);
	set = find_set(epfd);
	clock_gettime(CLOCK_MONOTONIC, &now);

	int ready = 0;
	for (int i = 0; set != NULL && i < count && ready < maxEvents; i++) {
		uint32_t readyEvents = 0;
		struct timer* timer = find_timer(snapshot[i].fd);
		if (timer != NULL) {
			if (timer->armed && (snapshot[i].events & EPOLLIN) != 0
				&& nanoseconds(&timer->deadline) <= nanoseconds(&now)) {
				readyEvents = EPOLLIN;
			}
		} else if (result > 0) {
			short revents = pollFDs[i].revents;
			if ((revents & POLLIN) != 0)
				readyEvents |= EPOLLIN;
			if ((revents & POLLOUT) != 0)
				readyEvents |= EPOLLOUT;
			if ((revents & POLLERR) != 0)
				readyEvents |= EPOLLERR;
			if ((revents & POLLHUP) != 0)
				readyEvents |= EPOLLHUP;
			if ((revents & POLLNVAL) != 0)
				readyEvents |= EPOLLERR;
		}
		if (readyEvents == 0)
			continue;

		// The set may have changed while this thread was not holding the
		// lock; an event for a source removed meanwhile must not be reported.
		bool stillRegistered = false;
		for (int j = 0; j < set->count; j++) {
			if (set->registrations[j].fd == snapshot[i].fd
				&& set->registrations[j].data.u64 == snapshot[i].data.u64) {
				stillRegistered = true;
				break;
			}
		}
		if (!stillRegistered)
			continue;

		events[ready].events = readyEvents;
		events[ready].data = snapshot[i].data;
		ready++;
	}
	pthread_mutex_unlock(&sLock);

	free(pollFDs);
	free(snapshot);
	return ready;
}


int
timerfd_create(clockid_t clock, int flags)
{
	if (clock != CLOCK_MONOTONIC) {
		errno = EINVAL;
		return -1;
	}

	int fd = open("/dev/null", O_RDONLY
		| ((flags & TFD_CLOEXEC) != 0 ? O_CLOEXEC : 0));
	if (fd < 0)
		return -1;

	struct timer* timer = calloc(1, sizeof(struct timer));
	if (timer == NULL) {
		close(fd);
		errno = ENOMEM;
		return -1;
	}
	timer->fd = fd;

	pthread_mutex_lock(&sLock);
	forget_fd(fd);
	timer->next = sTimers;
	sTimers = timer;
	pthread_mutex_unlock(&sLock);
	return fd;
}


int
timerfd_settime(int fd, int flags, const struct itimerspec* value,
	struct itimerspec* oldValue)
{
	pthread_mutex_lock(&sLock);
	struct timer* timer = find_timer(fd);
	if (timer == NULL) {
		pthread_mutex_unlock(&sLock);
		errno = EBADF;
		return -1;
	}

	if (oldValue != NULL)
		memset(oldValue, 0, sizeof(*oldValue));

	if (value->it_value.tv_sec == 0 && value->it_value.tv_nsec == 0) {
		timer->armed = false;
	} else {
		timer->deadline = value->it_value;
		if ((flags & TFD_TIMER_ABSTIME) == 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			int64_t deadline = nanoseconds(&now) + nanoseconds(&value->it_value);
			timer->deadline.tv_sec = deadline / 1000000000LL;
			timer->deadline.tv_nsec = deadline % 1000000000LL;
		}
		timer->armed = true;
	}
	pthread_mutex_unlock(&sLock);

	int epollFD = -1;
	pthread_mutex_lock(&sLock);
	for (struct epoll_set* set = sSets; set != NULL; set = set->next) {
		for (int i = 0; i < set->count; i++) {
			if (set->registrations[i].fd == fd) {
				epollFD = set->fd;
				break;
			}
		}
	}
	pthread_mutex_unlock(&sLock);

	// A loop already asleep computed its timeout from the old deadline.
	if (epollFD >= 0)
		wl_haiku_event_loop_wake(epollFD);
	return 0;
}


int
signalfd(int fd, const sigset_t* mask, int flags)
{
	errno = ENOSYS;
	return -1;
}


void
wl_haiku_event_loop_set_wait_hooks(void (*before)(void*),
	void (*after)(void*), void* data)
{
	sBeforeWait = before;
	sAfterWait = after;
	sWaitData = data;
}


void
wl_haiku_event_loop_wake(int epollFD)
{
	pthread_mutex_lock(&sLock);
	struct epoll_set* set = find_set(epollFD);
	int wakeFD = set != NULL ? set->wakeFD : -1;
	pthread_mutex_unlock(&sLock);

	if (wakeFD >= 0) {
		char byte = 0;
		write(wakeFD, &byte, 1);
	}
}
