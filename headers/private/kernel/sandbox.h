/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_SANDBOX_H
#define _KERNEL_SANDBOX_H


#include <pledge_defs.h>
#include <syscall_filter_defs.h>


/*!	Per-team confinement state, and the points the kernel consults it from.

	Two mechanisms share one piece of team state, because they answer the same
	question and a process will usually want both. pledge() and unveil() are
	the ones a Haiku program should reach for: they are written in terms of
	what the program does rather than which numbered call it makes, and they
	survive the syscall table being renumbered by the next build. The BPF
	filter exists so that a sandbox which already emits classic BPF -- the
	Chromium family's among them -- has an engine to run its policy on,
	instead of having that policy rewritten by hand for Haiku.

	The two are evaluated together and the more restrictive answer wins, so
	installing one can never loosen the other.
*/


struct team_sandbox;

namespace BKernel {
	struct Team;
}
using BKernel::Team;


#ifdef __cplusplus
extern "C" {
#endif


/*!	Builds the syscall-to-promise map.

	Called once during kernel startup, after the syscall table exists. The map
	is derived from the table's own names, so that it stays correct across the
	renumbering every build does.
*/
status_t	sandbox_init(void);

/*!	Releases \a team's confinement state. Safe on an unconfined team. */
void		sandbox_team_uninit(Team* team);

/*!	Copies \a parent's confinement onto \a child, for fork().

	A child that inherited nothing would be an escape hatch: a confined
	process could fork and have the copy do what it promised not to.

	\return \c B_OK, also when \a parent is unconfined and there is nothing to
		copy, or \c B_NO_MEMORY.
*/
status_t	sandbox_inherit(Team* child, Team* parent);

/*!	Decides what to do about a syscall from a confined thread.

	Only called for threads whose team carries confinement, which the
	THREAD_FLAGS_SANDBOXED flag records so the common path costs nothing.

	\param syscall The Haiku syscall index.
	\param args The argument block, as the dispatcher received it.
	\return \c SYSCALL_FILTER_RET_ALLOW to proceed, or the action to apply.
*/
uint32		sandbox_check_syscall(uint32 syscall, const void* args);

/*!	Applies the action \a sandbox_check_syscall() returned.

	Kills the team or the thread, raises SIGSYS, or arranges for the call to
	fail, as the action says.

	\return \c true if the syscall should still be performed.
*/
bool		sandbox_apply_action(uint32 action, uint32 syscall,
				uint64* _returnValue);

/*!	Tests an already-resolved path against the current team's unveil table.

	\param path An absolute, canonical path.
	\param access The UNVEIL_* access wanted.
	\return \c B_OK, or \c B_PERMISSION_DENIED.
*/
status_t	sandbox_check_path(const char* path, uint32 access);

/*!	Whether the current team holds \a promise. Unconfined teams hold all. */
bool		sandbox_has_promise(uint64 promise);


/*!	The syscall entry stub's entry point: check, act, and say whether the
	call should still go ahead. Only called for threads whose team is
	confined. */
bool		sandbox_pre_syscall(uint32 syscall, void* args,
				uint64* _returnValue);


// syscalls
status_t	_user_pledge(const char* promises, uint32 flags);
status_t	_user_unveil(const char* path, const char* permissions);
status_t	_user_set_syscall_filter(const syscall_filter_program* program,
				uint32 flags);
status_t	_user_syscall_index(const char* name, int32* _index);


#ifdef __cplusplus
}
#endif


#endif	/* _KERNEL_SANDBOX_H */
