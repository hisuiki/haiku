/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Sandbox.h>

#include <syscalls.h>


/*!	Thin wrappers over the confinement syscalls.

	Deliberately thin: none of these may retry, cache, or interpret what the
	kernel said. A caller that believes it is confined when it is not is worse
	off than one that never asked, so the kernel's answer is passed through
	exactly as given.
*/


status_t
pledge(const char* promises, uint32 flags)
{
	return _kern_pledge(promises, flags);
}


status_t
unveil(const char* path, const char* permissions)
{
	// The kernel reads a pair of NULLs as "no more paths", so a caller that
	// passed one NULL by accident must not fall into locking the table.
	if (path == NULL || permissions == NULL)
		return B_BAD_VALUE;

	return _kern_unveil(path, permissions);
}


status_t
unveil_lock(void)
{
	return _kern_unveil(NULL, NULL);
}


status_t
set_syscall_filter(const syscall_filter_program* program, uint32 flags)
{
	return _kern_set_syscall_filter(program, flags);
}


status_t
find_syscall_index(const char* name, int32* _index)
{
	return _kern_syscall_index(name, _index);
}
