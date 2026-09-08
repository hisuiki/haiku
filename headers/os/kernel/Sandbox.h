/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SANDBOX_H
#define _SANDBOX_H


#include <OS.h>

#include <pledge_defs.h>
#include <syscall_filter_defs.h>


/*!	Confining a process to what it actually needs.

	The three calls here are the userland face of one kernel mechanism, and
	they answer the same question from different directions:

	  - pledge() says what the process intends to do, in named capabilities.
	  - unveil() says which parts of the file system it intends to reach.
	  - set_syscall_filter() runs a classic BPF program against each call, for
	    policies that already exist in that form.

	Restrictions only ever accumulate. None of these can widen what an earlier
	call narrowed, and all of them survive fork() and exec(), so a confined
	process cannot shed its confinement by starting something else.
*/


#ifdef __cplusplus
extern "C" {
#endif


/*!	Restricts the process to \a promises.

	\param promises A space-separated list of promise names, or NULL to leave
		the current set alone. The empty string gives up everything but the
		calls a process needs to exit.
	\param flags PLEDGE_FLAG_ERROR to fail violating calls with ENOSYS rather
		than killing the team, PLEDGE_FLAG_LOG to log each one.
	\return \c B_OK, \c B_BAD_VALUE for an unknown promise name, or
		\c B_NOT_ALLOWED for an attempt to regain a promise already given up.
*/
extern status_t	pledge(const char* promises, uint32 flags);

/*!	Makes \a path, and everything below it, reachable with \a permissions.

	The first call hides the rest of the file system: from then on only what
	has been unveiled can be opened. A later call for a path already unveiled
	may narrow its permissions but never widen them.

	\param path An absolute path.
	\param permissions Some of "r", "w", "x" and "c"; the empty string hides a
		subtree that a broader entry would otherwise have covered.
	\return \c B_OK, or \c B_NOT_ALLOWED if the table is locked or the request
		would widen an existing entry.
*/
extern status_t	unveil(const char* path, const char* permissions);

/*!	Locks the unveil table, so that nothing loaded later can add to it. */
extern status_t	unveil_lock(void);

/*!	Installs a classic BPF program to be run before each system call.

	Programs accumulate, and the most restrictive answer among them wins.
	\a program is evaluated against a syscall_filter_data record holding a
	*Haiku* syscall index, which is unrelated to the Linux numbering the
	format otherwise matches.

	\return \c B_OK, or \c B_BAD_VALUE if the program could loop or could read
		outside that record.
*/
extern status_t	set_syscall_filter(const syscall_filter_program* program,
					uint32 flags);

/*!	Resolves a system call name, such as "_kern_open", to its index.

	A filter program compares against syscall_filter_data::nr, which holds a
	Haiku syscall index, and those are generated per build and move whenever a
	call is added. A policy therefore cannot carry numbers of its own: it has
	to ask for them at run time, which is what this is for.

	\return \c B_OK, or \c B_NAME_NOT_FOUND if this kernel has no such call.
*/
extern status_t	find_syscall_index(const char* name, int32* _index);


#ifdef __cplusplus
}
#endif


#endif	/* _SANDBOX_H */
