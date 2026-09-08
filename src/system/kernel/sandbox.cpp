/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sandbox.h>

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>

#include <AutoDeleter.h>
#include <fs/KPath.h>
#include <kernel.h>
#include <ksignal.h>
#include <ksyscalls.h>
#include <lock.h>
#include <team.h>
#include <thread.h>
#include <thread_types.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <vm/vm.h>


//#define TRACE_SANDBOX
#ifdef TRACE_SANDBOX
#	define TRACE(x...)	dprintf("sandbox: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif


/*!	The bit an unmapped syscall demands.

	No promise grants it, so a syscall the map below has no entry for is
	denied to every confined team. That is the safe direction to fail in: the
	alternative, allowing what has not been classified, would silently open a
	hole in every existing policy the first time a syscall is added.
*/
#define PLEDGE_UNMAPPED		0x8000000000000000ULL


namespace {


struct promise_mapping {
	const char*	syscall;
	uint64		promises;
};


/*!	What each system call needs, by name.

	Names rather than indices, because the indices are generated per build and
	move whenever a call is added: a table written against numbers would keep
	compiling and start guarding the wrong calls. sandbox_init() resolves
	these against the syscall table's own names once, at startup.

	A call is permitted when the team holds *any* of the promises listed for
	it. Several calls are genuinely two things at once -- write_stat is both a
	truncation and a chmod -- and splitting them by argument is the job of the
	finer checks in the VFS, not of a table keyed by call.
*/
const promise_mapping kPromiseMap[] = {
	// Always available. A confined process must be able to exit, to return
	// from a signal, and to say why it is unhappy.
	{ "_kern_exit_team",					PLEDGE_ALWAYS },
	{ "_kern_exit_thread",					PLEDGE_ALWAYS },
	{ "_kern_restore_signal_frame",			PLEDGE_ALWAYS },
	{ "_kern_debug_output",					PLEDGE_ALWAYS },
	{ "_kern_get_current_team",				PLEDGE_ALWAYS },
	{ "_kern_find_thread",					PLEDGE_ALWAYS },
	{ "_kern_system_time",					PLEDGE_ALWAYS },
	{ "_kern_is_computer_on",				PLEDGE_ALWAYS },

	// The confinement calls themselves. A process must be able to narrow
	// itself further after a first pledge -- that is the whole shape of the
	// pattern, where a program pledges widely at startup and again once it
	// has finished setting up -- and neither call can widen anything, so
	// leaving them reachable costs nothing. Looking up a syscall index has to
	// stay available for the same reason: a filter installed later is written
	// in terms of what that lookup returns.
	{ "_kern_pledge",						PLEDGE_ALWAYS },
	{ "_kern_unveil",						PLEDGE_ALWAYS },
	{ "_kern_set_syscall_filter",			PLEDGE_ALWAYS },
	{ "_kern_syscall_index",				PLEDGE_ALWAYS },

	// Basic runtime. Wider than OpenBSD's stdio because libroot builds its
	// heap out of areas and its locks out of semaphores: a team denied either
	// could not reach main() to be confined in the first place.
	{ "_kern_read",							PLEDGE_STDIO },
	{ "_kern_readv",						PLEDGE_STDIO },
	{ "_kern_write",						PLEDGE_STDIO },
	{ "_kern_writev",						PLEDGE_STDIO },
	{ "_kern_close",						PLEDGE_STDIO },
	{ "_kern_close_range",					PLEDGE_STDIO },
	{ "_kern_dup",							PLEDGE_STDIO },
	{ "_kern_dup2",							PLEDGE_STDIO },
	{ "_kern_seek",							PLEDGE_STDIO },
	{ "_kern_fcntl",						PLEDGE_STDIO },
	{ "_kern_fsync",						PLEDGE_STDIO },
	{ "_kern_sync",							PLEDGE_STDIO },
	{ "_kern_ioctl",						PLEDGE_STDIO },
		// Deliberately not gated here. ioctl is only as wide as the
		// descriptor it is given, and which descriptors exist is already
		// decided by the promises and the unveil table at open() time.
	{ "_kern_create_pipe",					PLEDGE_STDIO },
	{ "_kern_poll",							PLEDGE_STDIO },
	{ "_kern_select",						PLEDGE_STDIO },
	{ "_kern_wait_for_objects",				PLEDGE_STDIO },
	{ "_kern_event_queue_create",			PLEDGE_STDIO },
	{ "_kern_event_queue_select",			PLEDGE_STDIO },
	{ "_kern_event_queue_wait",				PLEDGE_STDIO },
	{ "_kern_mutex_lock",					PLEDGE_STDIO },
	{ "_kern_mutex_unblock",				PLEDGE_STDIO },
	{ "_kern_mutex_switch_lock",			PLEDGE_STDIO },
	{ "_kern_mutex_sem_acquire",			PLEDGE_STDIO },
	{ "_kern_mutex_sem_release",			PLEDGE_STDIO },
	{ "_kern_create_sem",					PLEDGE_STDIO },
	{ "_kern_delete_sem",					PLEDGE_STDIO },
	{ "_kern_acquire_sem",					PLEDGE_STDIO },
	{ "_kern_acquire_sem_etc",				PLEDGE_STDIO },
	{ "_kern_release_sem",					PLEDGE_STDIO },
	{ "_kern_release_sem_etc",				PLEDGE_STDIO },
	{ "_kern_switch_sem",					PLEDGE_STDIO },
	{ "_kern_switch_sem_etc",				PLEDGE_STDIO },
	{ "_kern_get_sem_count",				PLEDGE_STDIO },
	{ "_kern_get_sem_info",					PLEDGE_STDIO },
	{ "_kern_get_next_sem_info",			PLEDGE_STDIO | PLEDGE_PS },
	{ "_kern_set_sem_owner",				PLEDGE_STDIO },
	{ "_kern_create_area",					PLEDGE_STDIO },
	{ "_kern_delete_area",					PLEDGE_STDIO },
	{ "_kern_resize_area",					PLEDGE_STDIO },
	{ "_kern_set_area_protection",			PLEDGE_STDIO },
	{ "_kern_set_memory_protection",		PLEDGE_STDIO },
	{ "_kern_get_area_info",				PLEDGE_STDIO },
	{ "_kern_get_memory_properties",		PLEDGE_STDIO },
	{ "_kern_memory_advice",				PLEDGE_STDIO },
	{ "_kern_sync_memory",					PLEDGE_STDIO },
	{ "_kern_unmap_memory",					PLEDGE_STDIO },
	{ "_kern_reserve_address_range",		PLEDGE_STDIO },
	{ "_kern_unreserve_address_range",		PLEDGE_STDIO },
	{ "_kern_mlock",						PLEDGE_STDIO },
	{ "_kern_munlock",						PLEDGE_STDIO },
	{ "_kern_snooze_etc",					PLEDGE_STDIO },
	{ "_kern_thread_yield",					PLEDGE_STDIO },
	{ "_kern_block_thread",					PLEDGE_STDIO },
	{ "_kern_unblock_thread",				PLEDGE_STDIO },
	{ "_kern_unblock_threads",				PLEDGE_STDIO },
	{ "_kern_sigaction",					PLEDGE_STDIO },
	{ "_kern_sigpending",					PLEDGE_STDIO },
	{ "_kern_sigsuspend",					PLEDGE_STDIO },
	{ "_kern_sigwait",						PLEDGE_STDIO },
	{ "_kern_set_signal_mask",				PLEDGE_STDIO },
	{ "_kern_set_signal_stack",				PLEDGE_STDIO },
	{ "_kern_getrlimit",					PLEDGE_STDIO },
	{ "_kern_get_system_info",				PLEDGE_STDIO },
	{ "_kern_get_cpu",						PLEDGE_STDIO },
	{ "_kern_get_cpuid",					PLEDGE_STDIO },
	{ "_kern_get_cpu_info",					PLEDGE_STDIO },
	{ "_kern_get_cpu_topology_info",		PLEDGE_STDIO },
	{ "_kern_cpu_enabled",					PLEDGE_STDIO },
	{ "_kern_get_cpuclockid",				PLEDGE_STDIO },
	{ "_kern_get_clock",					PLEDGE_STDIO },
	{ "_kern_get_timer",					PLEDGE_STDIO },
	{ "_kern_set_timer",					PLEDGE_STDIO },
	{ "_kern_create_timer",					PLEDGE_STDIO },
	{ "_kern_delete_timer",					PLEDGE_STDIO },
	{ "_kern_get_timezone",					PLEDGE_STDIO },
	{ "_kern_get_real_time_clock_is_gmt",	PLEDGE_STDIO },
	{ "_kern_get_safemode_option",			PLEDGE_STDIO },
	{ "_kern_get_loadavg",					PLEDGE_STDIO | PLEDGE_VMINFO },
	{ "_kern_estimate_max_scheduling_latency", PLEDGE_STDIO },
	{ "_kern_clear_caches",					PLEDGE_STDIO },
	{ "_kern_getcwd",						PLEDGE_STDIO },
	{ "_kern_getgroups",					PLEDGE_STDIO },
	{ "_kern_getresuid",					PLEDGE_STDIO },
	{ "_kern_getresgid",					PLEDGE_STDIO },
	{ "_kern_process_info",					PLEDGE_STDIO },
	{ "_kern_get_next_fd_info",				PLEDGE_STDIO | PLEDGE_PS },
	{ "_kern_get_thread_info",				PLEDGE_STDIO },
	{ "_kern_get_scheduler_mode",			PLEDGE_STDIO },

	// The runtime loader runs before any pledge can be made, but it also runs
	// again for every add-on the team loads afterwards. Bookkeeping calls stay
	// open; what actually gates loading code is the rpath promise and the
	// unveil table, which the open() and map_file() below have to pass first.
	{ "_kern_register_image",				PLEDGE_STDIO },
	{ "_kern_unregister_image",				PLEDGE_STDIO },
	{ "_kern_image_relocated",				PLEDGE_STDIO },
	{ "_kern_loading_app_failed",			PLEDGE_STDIO },
	{ "_kern_get_image_info",				PLEDGE_STDIO },
	{ "_kern_get_next_image_info",			PLEDGE_STDIO | PLEDGE_PS },
	{ "_kern_read_kernel_image_symbols",	PLEDGE_DEBUG },

	// Reading the file system.
	{ "_kern_open",							PLEDGE_RPATH | PLEDGE_WPATH
												| PLEDGE_CPATH
												| PLEDGE_TMPPATH },
	{ "_kern_open_entry_ref",				PLEDGE_RPATH | PLEDGE_WPATH
												| PLEDGE_CPATH
												| PLEDGE_TMPPATH },
	{ "_kern_open_dir",						PLEDGE_RPATH },
	{ "_kern_open_dir_entry_ref",			PLEDGE_RPATH },
	{ "_kern_open_parent_dir",				PLEDGE_RPATH },
	{ "_kern_read_dir",						PLEDGE_RPATH },
	{ "_kern_rewind_dir",					PLEDGE_RPATH },
	{ "_kern_read_stat",					PLEDGE_RPATH },
	{ "_kern_read_link",					PLEDGE_RPATH },
	{ "_kern_access",						PLEDGE_RPATH },
	{ "_kern_read_fs_info",					PLEDGE_RPATH },
	{ "_kern_setcwd",						PLEDGE_RPATH },
	{ "_kern_normalize_path",				PLEDGE_RPATH },
	{ "_kern_entry_ref_to_path",			PLEDGE_RPATH },
	{ "_kern_map_file",						PLEDGE_RPATH },

	// Changing it.
	{ "_kern_write_stat",					PLEDGE_WPATH | PLEDGE_FATTR },
	{ "_kern_write_fs_info",				PLEDGE_WPATH },
	{ "_kern_preallocate",					PLEDGE_WPATH },
	{ "_kern_create_dir",					PLEDGE_CPATH },
	{ "_kern_create_dir_entry_ref",			PLEDGE_CPATH },
	{ "_kern_create_symlink",				PLEDGE_CPATH },
	{ "_kern_create_link",					PLEDGE_CPATH },
	{ "_kern_create_fifo",					PLEDGE_CPATH | PLEDGE_DPATH },
	{ "_kern_remove_dir",					PLEDGE_CPATH },
	{ "_kern_unlink",						PLEDGE_CPATH },
	{ "_kern_rename",						PLEDGE_CPATH },
	{ "_kern_flock",						PLEDGE_FLOCK },
	{ "_kern_lock_node",					PLEDGE_FLOCK },
	{ "_kern_unlock_node",					PLEDGE_FLOCK },

	// BFS features with no POSIX equivalent.
	{ "_kern_open_attr",					PLEDGE_ATTR },
	{ "_kern_open_attr_dir",				PLEDGE_ATTR },
	{ "_kern_read_attr",					PLEDGE_ATTR },
	{ "_kern_write_attr",					PLEDGE_ATTR },
	{ "_kern_remove_attr",					PLEDGE_ATTR },
	{ "_kern_rename_attr",					PLEDGE_ATTR },
	{ "_kern_stat_attr",					PLEDGE_ATTR },
	{ "_kern_create_index",					PLEDGE_INDEX },
	{ "_kern_remove_index",					PLEDGE_INDEX },
	{ "_kern_open_index_dir",				PLEDGE_INDEX },
	{ "_kern_read_index_stat",				PLEDGE_INDEX },
	{ "_kern_open_query",					PLEDGE_QUERY },
	{ "_kern_start_watching",				PLEDGE_NODE_MONITOR },
	{ "_kern_stop_watching",				PLEDGE_NODE_MONITOR },
	{ "_kern_stop_notifying",				PLEDGE_NODE_MONITOR },

	// Sockets. Which address families a socket may use is decided by
	// _user_socket() itself, since only it can see the family argument.
	{ "_kern_socket",						PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_socketpair",					PLEDGE_UNIX },
	{ "_kern_bind",							PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_listen",						PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_accept",						PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_connect",						PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS },
	{ "_kern_send",							PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS },
	{ "_kern_sendto",						PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS },
	{ "_kern_sendmsg",						PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS
												| PLEDGE_SENDFD },
	{ "_kern_recv",							PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS },
	{ "_kern_recvfrom",						PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS },
	{ "_kern_recvmsg",						PLEDGE_INET | PLEDGE_UNIX
												| PLEDGE_DNS
												| PLEDGE_RECVFD },
	{ "_kern_getsockname",					PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_getpeername",					PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_getsockopt",					PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_setsockopt",					PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_shutdown_socket",				PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_sockatmark",					PLEDGE_INET | PLEDGE_UNIX },
	{ "_kern_get_next_socket_stat",			PLEDGE_PS },

	// Processes, threads and images.
	{ "_kern_fork",							PLEDGE_PROC },
	{ "_kern_wait_for_child",				PLEDGE_PROC },
	{ "_kern_wait_for_team",				PLEDGE_PROC },
	{ "_kern_send_signal",					PLEDGE_PROC | PLEDGE_THREAD },
	{ "_kern_kill_team",					PLEDGE_PROC },
	{ "_kern_setpgid",						PLEDGE_PROC },
	{ "_kern_setsid",						PLEDGE_PROC },
	{ "_kern_exec",							PLEDGE_EXEC },
	{ "_kern_load_image",					PLEDGE_EXEC },
	{ "_kern_spawn_thread",					PLEDGE_THREAD },
	{ "_kern_resume_thread",				PLEDGE_THREAD },
	{ "_kern_suspend_thread",				PLEDGE_THREAD },
	{ "_kern_kill_thread",					PLEDGE_THREAD },
	{ "_kern_cancel_thread",				PLEDGE_THREAD },
	{ "_kern_wait_for_thread_etc",			PLEDGE_THREAD },
	{ "_kern_rename_thread",				PLEDGE_THREAD },
	{ "_kern_set_thread_priority",			PLEDGE_THREAD },
	{ "_kern_get_thread_affinity",			PLEDGE_THREAD },
	{ "_kern_set_thread_affinity",			PLEDGE_THREAD },
	{ "_kern_setresuid",					PLEDGE_ID },
	{ "_kern_setresgid",					PLEDGE_ID },
	{ "_kern_setgroups",					PLEDGE_ID },
	{ "_kern_setrlimit",					PLEDGE_ID },

	// Looking at the rest of the system.
	{ "_kern_get_team_info",				PLEDGE_PS },
	{ "_kern_get_next_team_info",			PLEDGE_PS },
	{ "_kern_get_extended_team_info",		PLEDGE_PS },
	{ "_kern_get_team_usage_info",			PLEDGE_PS },
	{ "_kern_get_next_thread_info",			PLEDGE_PS },
	{ "_kern_get_next_area_info",			PLEDGE_VMINFO | PLEDGE_PS },
	{ "_kern_set_clock",					PLEDGE_SETTIME },
	{ "_kern_set_real_time_clock",			PLEDGE_SETTIME },
	{ "_kern_set_real_time_clock_is_gmt",	PLEDGE_SETTIME },
	{ "_kern_set_timezone",					PLEDGE_SETTIME },

	// Ports and areas: the transport under every BMessage, and the way memory
	// is shared between teams.
	{ "_kern_create_port",					PLEDGE_PORT },
	{ "_kern_delete_port",					PLEDGE_PORT },
	{ "_kern_close_port",					PLEDGE_PORT },
	{ "_kern_find_port",					PLEDGE_PORT },
	{ "_kern_get_port_info",				PLEDGE_PORT },
	{ "_kern_get_next_port_info",			PLEDGE_PORT | PLEDGE_PS },
	{ "_kern_get_port_message_info_etc",	PLEDGE_PORT },
	{ "_kern_port_buffer_size_etc",			PLEDGE_PORT },
	{ "_kern_port_count",					PLEDGE_PORT },
	{ "_kern_read_port_etc",				PLEDGE_PORT },
	{ "_kern_write_port_etc",				PLEDGE_PORT },
	{ "_kern_writev_port_etc",				PLEDGE_PORT },
	{ "_kern_set_port_owner",				PLEDGE_PORT },
	{ "_kern_has_data",						PLEDGE_PORT },
	{ "_kern_send_data",					PLEDGE_PORT },
	{ "_kern_receive_data",					PLEDGE_PORT },
	{ "_kern_clone_area",					PLEDGE_AREA },
	{ "_kern_find_area",					PLEDGE_AREA },
	{ "_kern_transfer_area",				PLEDGE_AREA },

	// Named and XSI semaphores, and XSI message queues: shared namespaces
	// that outlive the process, and so a way out of a confinement.
	{ "_kern_realtime_sem_open",			PLEDGE_SEM },
	{ "_kern_realtime_sem_close",			PLEDGE_SEM },
	{ "_kern_realtime_sem_unlink",			PLEDGE_SEM },
	{ "_kern_realtime_sem_wait",			PLEDGE_SEM },
	{ "_kern_realtime_sem_post",			PLEDGE_SEM },
	{ "_kern_realtime_sem_get_value",		PLEDGE_SEM },
	{ "_kern_xsi_semget",					PLEDGE_SEM },
	{ "_kern_xsi_semctl",					PLEDGE_SEM },
	{ "_kern_xsi_semop",					PLEDGE_SEM },
	{ "_kern_xsi_msgget",					PLEDGE_SEM },
	{ "_kern_xsi_msgctl",					PLEDGE_SEM },
	{ "_kern_xsi_msgsnd",					PLEDGE_SEM },
	{ "_kern_xsi_msgrcv",					PLEDGE_SEM },

	// The disk device manager and the mount table.
	{ "_kern_next_device",					PLEDGE_DISK },
	{ "_kern_find_disk_device",				PLEDGE_DISK },
	{ "_kern_find_disk_system",				PLEDGE_DISK },
	{ "_kern_find_file_disk_device",		PLEDGE_DISK },
	{ "_kern_find_partition",				PLEDGE_DISK },
	{ "_kern_get_disk_device_data",			PLEDGE_DISK },
	{ "_kern_get_disk_system_info",			PLEDGE_DISK },
	{ "_kern_get_next_disk_device_id",		PLEDGE_DISK },
	{ "_kern_get_next_disk_system_info",	PLEDGE_DISK },
	{ "_kern_get_file_disk_device_path",	PLEDGE_DISK },
	{ "_kern_register_file_device",			PLEDGE_DISK },
	{ "_kern_unregister_file_device",		PLEDGE_DISK },
	{ "_kern_start_watching_disks",			PLEDGE_DISK },
	{ "_kern_stop_watching_disks",			PLEDGE_DISK },
	{ "_kern_defragment_partition",			PLEDGE_DISK },
	{ "_kern_repair_partition",				PLEDGE_DISK },
	{ "_kern_resize_partition",				PLEDGE_DISK },
	{ "_kern_move_partition",				PLEDGE_DISK },
	{ "_kern_initialize_partition",			PLEDGE_DISK },
	{ "_kern_uninitialize_partition",		PLEDGE_DISK },
	{ "_kern_create_child_partition",		PLEDGE_DISK },
	{ "_kern_delete_child_partition",		PLEDGE_DISK },
	{ "_kern_set_partition_name",			PLEDGE_DISK },
	{ "_kern_set_partition_type",			PLEDGE_DISK },
	{ "_kern_set_partition_parameters",		PLEDGE_DISK },
	{ "_kern_set_partition_content_name",	PLEDGE_DISK },
	{ "_kern_set_partition_content_parameters", PLEDGE_DISK },
	{ "_kern_mount",						PLEDGE_MOUNT },
	{ "_kern_unmount",						PLEDGE_MOUNT },
	{ "_kern_change_root",					PLEDGE_MOUNT },

	// Debugging another team is a way to run code inside it, so it has to be
	// as tightly held as anything here.
	{ "_kern_install_team_debugger",		PLEDGE_DEBUG },
	{ "_kern_remove_team_debugger",			PLEDGE_DEBUG },
	{ "_kern_install_default_debugger",		PLEDGE_DEBUG },
	{ "_kern_debug_thread",					PLEDGE_DEBUG },
	{ "_kern_wait_for_debugger",			PLEDGE_DEBUG },
	{ "_kern_set_debugger_breakpoint",		PLEDGE_DEBUG },
	{ "_kern_clear_debugger_breakpoint",	PLEDGE_DEBUG },
	{ "_kern_disable_debugger",				PLEDGE_DEBUG },
	{ "_kern_debugger",						PLEDGE_DEBUG },
	{ "_kern_kernel_debugger",				PLEDGE_DEBUG },
	{ "_kern_ktrace_output",				PLEDGE_DEBUG },
	{ "_kern_analyze_scheduling",			PLEDGE_DEBUG },
	{ "_kern_system_profiler_start",		PLEDGE_DEBUG },
	{ "_kern_system_profiler_stop",			PLEDGE_DEBUG },
	{ "_kern_system_profiler_next_buffer",	PLEDGE_DEBUG },
	{ "_kern_system_profiler_recorded",		PLEDGE_DEBUG },

	// Changing how the kernel itself behaves.
	{ "_kern_generic_syscall",				PLEDGE_SETTINGS },
	{ "_kern_shutdown",						PLEDGE_SETTINGS },
	{ "_kern_set_cpu_enabled",				PLEDGE_SETTINGS },
	{ "_kern_set_scheduler_mode",			PLEDGE_SETTINGS },
	{ "_kern_register_messaging_service",	PLEDGE_SETTINGS },
	{ "_kern_unregister_messaging_service",	PLEDGE_SETTINGS },
	{ "_kern_register_syslog_daemon",		PLEDGE_SETTINGS },
	{ "_kern_frame_buffer_update",			PLEDGE_SETTINGS },
	{ "_kern_start_watching_system",		PLEDGE_SETTINGS },
	{ "_kern_stop_watching_system",			PLEDGE_SETTINGS },
};

const size_t kPromiseMapSize = sizeof(kPromiseMap) / sizeof(kPromiseMap[0]);


/*!	Linux errno numbers, and what a refused call returns for each.

	A BPF policy carries the errno it wants in 16 bits, which only works for
	Linux's small positive numbering; Haiku's own E* constants are the B_*
	codes and do not fit. Translating here rather than at the edges means an
	unmodified policy produces the error it meant to, and an unrecognised
	number produces a refusal rather than a nonsense status.
*/
struct errno_mapping {
	uint32		linux_errno;
	status_t	status;
};

const errno_mapping kErrnoMap[] = {
	{ SYSCALL_FILTER_ERRNO_EPERM,	B_NOT_ALLOWED },
	{ SYSCALL_FILTER_ERRNO_ENOENT,	B_ENTRY_NOT_FOUND },
	{ SYSCALL_FILTER_ERRNO_EBADF,	B_FILE_ERROR },
	{ SYSCALL_FILTER_ERRNO_EAGAIN,	B_WOULD_BLOCK },
	{ SYSCALL_FILTER_ERRNO_ENOMEM,	B_NO_MEMORY },
	{ SYSCALL_FILTER_ERRNO_EACCES,	B_PERMISSION_DENIED },
	{ SYSCALL_FILTER_ERRNO_EFAULT,	B_BAD_ADDRESS },
	{ SYSCALL_FILTER_ERRNO_EINVAL,	B_BAD_VALUE },
	{ SYSCALL_FILTER_ERRNO_ENOSYS,	B_NOT_SUPPORTED },
};

const size_t kErrnoMapSize = sizeof(kErrnoMap) / sizeof(kErrnoMap[0]);


/*!	One installed BPF program.

	The list a team carries is only ever appended to, and nothing is released
	before the team dies, so the check path can walk it without taking a lock.
*/
struct filter_entry {
	filter_entry*			next;
	uint32					length;
	uint32					flags;
	syscall_filter_insn*	instructions;
};


}	// unnamed namespace


/*!	Everything a confined team carries.

	Allocated on the first pledge(), unveil() or filter installation, and
	freed with the team. \c promises is read on every syscall from the team's
	own threads while another of them may be narrowing it, so it is only ever
	written with an atomic that clears bits: a reader either sees the wider
	set or the narrower one, and both are answers the caller could legally
	have got by racing the two calls.
*/
struct team_sandbox {
	mutex			lock;
	uint64			promises;
	uint32			flags;
	filter_entry*	filters;
	unveil_entry*	unveil_entries;
	size_t			unveil_count;
	bool			unveil_locked;
};


static uint64* sSyscallPromises = NULL;


// #pragma mark - state


static team_sandbox*
create_sandbox()
{
	team_sandbox* sandbox = (team_sandbox*)malloc(sizeof(team_sandbox));
	if (sandbox == NULL)
		return NULL;

	mutex_init(&sandbox->lock, "team sandbox");
	sandbox->promises = PLEDGE_ALL_PROMISES;
	sandbox->flags = 0;
	sandbox->filters = NULL;
	sandbox->unveil_entries = NULL;
	sandbox->unveil_count = 0;
	sandbox->unveil_locked = false;

	return sandbox;
}


static void
delete_sandbox(team_sandbox* sandbox)
{
	if (sandbox == NULL)
		return;

	while (filter_entry* filter = sandbox->filters) {
		sandbox->filters = filter->next;
		free(filter->instructions);
		free(filter);
	}

	for (size_t i = 0; i < sandbox->unveil_count; i++)
		free(sandbox->unveil_entries[i].path);
	free(sandbox->unveil_entries);

	mutex_destroy(&sandbox->lock);
	free(sandbox);
}


/*!	Returns the calling team's sandbox, creating it if there is none.

	The team lock is held while the pointer is published, so two threads
	pledging at once cannot each install a sandbox and have one of them lose
	the other's restrictions.
*/
static team_sandbox*
get_or_create_team_sandbox(Team* team)
{
	TeamLocker teamLocker(team);

	if (team->sandbox != NULL)
		return team->sandbox;

	team_sandbox* sandbox = create_sandbox();
	if (sandbox == NULL)
		return NULL;

	team->sandbox = sandbox;
	return sandbox;
}


/*!	Marks every thread of \a team as needing the syscall check.

	The flag is what keeps the cost off the common path: the syscall entry
	stub tests it and calls none of this for an unconfined thread.
*/
static void
mark_team_sandboxed(Team* team)
{
	InterruptsSpinLocker threadCreationLocker(gThreadCreationLock);

	for (Thread* thread = team->thread_list.First(); thread != NULL;
			thread = team->thread_list.GetNext(thread)) {
		atomic_or(&thread->flags, THREAD_FLAGS_SANDBOXED);
	}
}


// #pragma mark - kernel interface


status_t
sandbox_init(void)
{
	sSyscallPromises = (uint64*)malloc(sizeof(uint64) * kSyscallCount);
	if (sSyscallPromises == NULL)
		return B_NO_MEMORY;

	for (int i = 0; i < kSyscallCount; i++)
		sSyscallPromises[i] = PLEDGE_UNMAPPED;

	for (size_t i = 0; i < kPromiseMapSize; i++) {
		bool found = false;

		for (int j = 0; j < kSyscallCount; j++) {
			if (strcmp(kExtendedSyscallInfos[j].name, kPromiseMap[i].syscall)
					== 0) {
				sSyscallPromises[j] = kPromiseMap[i].promises;
				found = true;
				break;
			}
		}

		// A name that no longer exists means the table has drifted from the
		// syscall list. Worth saying out loud, but not worth refusing to
		// boot over: the effect is only that some other call stays unmapped.
		if (!found) {
			dprintf("sandbox: promise map names a syscall that does not "
				"exist: %s\n", kPromiseMap[i].syscall);
		}
	}

	// The other direction matters more: an unmapped call is denied outright,
	// so a policy that used to work would start failing after the call was
	// added. Name them all, so it is obvious what to classify.
	for (int i = 0; i < kSyscallCount; i++) {
		if (sSyscallPromises[i] == PLEDGE_UNMAPPED) {
			dprintf("sandbox: no promise maps %s; it is denied to confined "
				"teams\n", kExtendedSyscallInfos[i].name);
		}
	}

	return B_OK;
}


void
sandbox_team_uninit(Team* team)
{
	delete_sandbox(team->sandbox);
	team->sandbox = NULL;
}


status_t
sandbox_inherit(Team* child, Team* parent)
{
	if (parent->sandbox == NULL)
		return B_OK;

	team_sandbox* source = parent->sandbox;
	MutexLocker sourceLocker(source->lock);

	team_sandbox* sandbox = create_sandbox();
	if (sandbox == NULL)
		return B_NO_MEMORY;

	sandbox->promises = source->promises;
	sandbox->flags = source->flags;
	sandbox->unveil_locked = source->unveil_locked;

	// Deep-copy rather than share: the child may narrow itself further, and
	// doing so must not reach back into the parent.
	if (source->unveil_count > 0) {
		sandbox->unveil_entries = (unveil_entry*)malloc(
			sizeof(unveil_entry) * source->unveil_count);
		if (sandbox->unveil_entries == NULL) {
			delete_sandbox(sandbox);
			return B_NO_MEMORY;
		}

		for (size_t i = 0; i < source->unveil_count; i++) {
			unveil_entry& entry = sandbox->unveil_entries[i];
			entry.path = strdup(source->unveil_entries[i].path);
			entry.path_length = source->unveil_entries[i].path_length;
			entry.permissions = source->unveil_entries[i].permissions;

			if (entry.path == NULL) {
				// Everything copied so far is already owned by the new
				// sandbox, so account for it before tearing that down.
				sandbox->unveil_count = i;
				delete_sandbox(sandbox);
				return B_NO_MEMORY;
			}
		}

		sandbox->unveil_count = source->unveil_count;
	}

	// Filters are copied in installation order, so that the head of the new
	// list is the most recently installed one, as in the parent.
	filter_entry** tail = &sandbox->filters;
	for (filter_entry* filter = source->filters; filter != NULL;
			filter = filter->next) {
		filter_entry* copy = (filter_entry*)malloc(sizeof(filter_entry));
		if (copy == NULL) {
			delete_sandbox(sandbox);
			return B_NO_MEMORY;
		}

		const size_t size = sizeof(syscall_filter_insn) * filter->length;
		copy->next = NULL;
		copy->length = filter->length;
		copy->flags = filter->flags;
		copy->instructions = (syscall_filter_insn*)malloc(size);
		if (copy->instructions == NULL) {
			free(copy);
			delete_sandbox(sandbox);
			return B_NO_MEMORY;
		}

		memcpy(copy->instructions, filter->instructions, size);
		*tail = copy;
		tail = &copy->next;
	}

	child->sandbox = sandbox;
	return B_OK;
}


bool
sandbox_has_promise(uint64 promise)
{
	Team* team = thread_get_current_thread()->team;
	if (team->sandbox == NULL)
		return true;

	return (atomic_get64((int64*)&team->sandbox->promises) & promise) != 0;
}


uint32
sandbox_check_syscall(uint32 syscall, const void* args)
{
	Thread* thread = thread_get_current_thread();
	Team* team = thread->team;
	team_sandbox* sandbox = team->sandbox;

	// sSyscallPromises is NULL only if sandbox_init() could not allocate it,
	// which leaves promises unenforceable; filters still work, so fall
	// through to them rather than refusing everything.
	if (sandbox == NULL || syscall >= (uint32)kSyscallCount)
		return SYSCALL_FILTER_RET_ALLOW;

	uint32 action = SYSCALL_FILTER_RET_ALLOW;

	// Promises first: they are a single load and a mask, and they are what a
	// Haiku policy will usually be written in.
	const uint64 promises = (uint64)atomic_get64((int64*)&sandbox->promises);
	if (sSyscallPromises != NULL
		&& (sSyscallPromises[syscall] & promises) == 0) {
		action = (sandbox->flags & PLEDGE_FLAG_ERROR) != 0
			? (SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_ENOSYS)
			: SYSCALL_FILTER_RET_KILL_PROCESS;

		if ((sandbox->flags & PLEDGE_FLAG_LOG) != 0) {
			dprintf("sandbox: team %" B_PRId32 " (%s) broke its pledge on "
				"%s\n", team->id, team->Name(),
				kExtendedSyscallInfos[syscall].name);
		}
	}

	if (sandbox->filters == NULL)
		return action;

	syscall_filter_data data;
	memset(&data, 0, sizeof(data));
	data.nr = (int32)syscall;
#ifdef __x86_64__
#	ifdef _COMPAT_MODE
	// A 32-bit process on a 64-bit kernel reaches the same syscall table, but
	// its ABI is not the one a policy written for x86_64 was checked against.
	data.arch = (thread->flags & THREAD_FLAGS_COMPAT_MODE) != 0
		? SYSCALL_FILTER_ARCH_X86 : SYSCALL_FILTER_ARCH_X86_64;
#	else
	data.arch = SYSCALL_FILTER_ARCH_X86_64;
#	endif
#elif defined(__i386__)
	data.arch = SYSCALL_FILTER_ARCH_X86;
#elif defined(__aarch64__)
	data.arch = SYSCALL_FILTER_ARCH_ARM64;
#elif defined(__arm__)
	data.arch = SYSCALL_FILTER_ARCH_ARM;
#elif defined(__riscv)
	data.arch = SYSCALL_FILTER_ARCH_RISCV64;
#endif

	// The dispatcher hands over the arguments as a flat block of machine
	// words, which is the shape a BPF policy expects to index.
	const int parameterSize = kSyscallInfos[syscall].parameter_size;
	if (args != NULL && parameterSize > 0) {
		size_t words = (size_t)parameterSize / sizeof(uint64);
		if (words > 6)
			words = 6;
		memcpy(data.args, args, words * sizeof(uint64));
	}

	for (filter_entry* filter = sandbox->filters; filter != NULL;
			filter = filter->next) {
		const uint32 result = syscall_filter_run(filter->instructions,
			filter->length, &data);

		if ((filter->flags & SYSCALL_FILTER_FLAG_LOG) != 0
			&& (result & SYSCALL_FILTER_RET_ACTION_MASK)
				!= SYSCALL_FILTER_RET_ALLOW) {
			dprintf("sandbox: team %" B_PRId32 " (%s) filtered on %s -> %#"
				B_PRIx32 "\n", team->id, team->Name(),
				kExtendedSyscallInfos[syscall].name, result);
		}

		action = syscall_filter_combine(action, result);
	}

	return action;
}


bool
sandbox_apply_action(uint32 action, uint32 syscall, uint64* _returnValue)
{
	switch (action & SYSCALL_FILTER_RET_ACTION_MASK) {
		case SYSCALL_FILTER_RET_ALLOW:
		case SYSCALL_FILTER_RET_LOG:
			return true;

		case SYSCALL_FILTER_RET_ERRNO:
		{
			const uint32 number = action & SYSCALL_FILTER_RET_DATA_MASK;
			status_t status = B_NOT_ALLOWED;

			for (size_t i = 0; i < kErrnoMapSize; i++) {
				if (kErrnoMap[i].linux_errno == number) {
					status = kErrnoMap[i].status;
					break;
				}
			}

			*_returnValue = (uint64)(int64)status;
			return false;
		}

		case SYSCALL_FILTER_RET_TRAP:
		{
			Thread* thread = thread_get_current_thread();
			Signal signal(SIGSYS, SI_USER, B_OK, thread->team->id);
			send_signal_to_thread(thread, signal, 0);
			*_returnValue = (uint64)(int64)B_PERMISSION_DENIED;
			return false;
		}

		case SYSCALL_FILTER_RET_TRACE:
			// Without a debugger attached there is nobody to make the
			// decision, and defaulting to "allow" would let a process escape
			// by simply not being traced.
			// fall through

		case SYSCALL_FILTER_RET_KILL_THREAD:
		{
			Thread* thread = thread_get_current_thread();
			Signal signal(SIGKILLTHR, SI_USER, B_OK, thread->team->id);
			send_signal_to_thread(thread, signal, 0);
			*_returnValue = (uint64)(int64)B_PERMISSION_DENIED;
			return false;
		}

		case SYSCALL_FILTER_RET_KILL_PROCESS:
		default:
		{
			Team* team = thread_get_current_thread()->team;
			dprintf("sandbox: killing team %" B_PRId32 " (%s) for %s\n",
				team->id, team->Name(),
				syscall < (uint32)kSyscallCount
					? kExtendedSyscallInfos[syscall].name : "<unknown>");

			Signal signal(SIGKILL, SI_USER, B_OK, team->id);
			send_signal_to_team(team, signal, 0);
			*_returnValue = (uint64)(int64)B_PERMISSION_DENIED;
			return false;
		}
	}
}


status_t
sandbox_check_path(const char* path, uint32 access)
{
	Team* team = thread_get_current_thread()->team;
	team_sandbox* sandbox = team->sandbox;

	if (sandbox == NULL || path == NULL)
		return B_OK;

	MutexLocker locker(sandbox->lock);

	if (sandbox->unveil_count == 0)
		return B_OK;

	if (unveil_check_access(sandbox->unveil_entries, sandbox->unveil_count,
			path, access)) {
		return B_OK;
	}

	if ((sandbox->flags & PLEDGE_FLAG_LOG) != 0) {
		dprintf("sandbox: team %" B_PRId32 " (%s) denied %#" B_PRIx32
			" on %s\n", team->id, team->Name(), access, path);
	}

	return B_PERMISSION_DENIED;
}


/*!	The syscall entry stub's single entry point into all of this.

	Kept as one call because the stub has to build an argument block and save
	a register around it, and doing that twice -- once to decide, once to act
	-- would cost every confined syscall a second detour.

	\param syscall The Haiku syscall index.
	\param args The flat argument block the dispatcher will pass on.
	\param _returnValue Set to what the syscall should return when it is
		refused; untouched otherwise.
	\return \c true if the syscall should be performed.
*/
extern "C" bool
sandbox_pre_syscall(uint32 syscall, void* args, uint64* _returnValue)
{
	const uint32 action = sandbox_check_syscall(syscall, args);

	// The overwhelmingly common case for a confined team is still that the
	// call is allowed, so keep that out of the switch in sandbox_apply_action.
	if (action == SYSCALL_FILTER_RET_ALLOW)
		return true;

	return sandbox_apply_action(action, syscall, _returnValue);
}


// #pragma mark - syscalls


status_t
_user_pledge(const char* userPromises, uint32 flags)
{
	char promises[512];

	if (userPromises != NULL) {
		if (!IS_USER_ADDRESS(userPromises))
			return B_BAD_ADDRESS;

		if (user_strlcpy(promises, userPromises, sizeof(promises)) < 0)
			return B_BAD_ADDRESS;
	}

	uint64 mask;
	status_t status = pledge_parse_promises(
		userPromises != NULL ? promises : NULL, &mask);
	if (status != B_OK)
		return status;

	Team* team = thread_get_current_thread()->team;
	team_sandbox* sandbox = get_or_create_team_sandbox(team);
	if (sandbox == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(sandbox->lock);

	// Narrowing only. A team that could re-pledge a promise it had dropped
	// could undo its own confinement, which is the whole thing this is for.
	if ((mask & ~sandbox->promises) != 0)
		return B_NOT_ALLOWED;

	sandbox->promises = mask;
	sandbox->flags |= flags;

	locker.Unlock();

	mark_team_sandboxed(team);
	return B_OK;
}


status_t
_user_unveil(const char* userPath, const char* userPermissions)
{
	Team* team = thread_get_current_thread()->team;

	// unveil(NULL, NULL) is the way a process says it is done adding paths.
	// Without it, code that runs later could widen the table it inherited.
	if (userPath == NULL && userPermissions == NULL) {
		team_sandbox* sandbox = get_or_create_team_sandbox(team);
		if (sandbox == NULL)
			return B_NO_MEMORY;

		MutexLocker locker(sandbox->lock);
		sandbox->unveil_locked = true;

		locker.Unlock();
		mark_team_sandboxed(team);
		return B_OK;
	}

	if (userPath == NULL || userPermissions == NULL)
		return B_BAD_VALUE;
	if (!IS_USER_ADDRESS(userPath) || !IS_USER_ADDRESS(userPermissions))
		return B_BAD_ADDRESS;

	KPath path;
	if (path.InitCheck() != B_OK)
		return B_NO_MEMORY;

	if (user_strlcpy(path.LockBuffer(), userPath, path.BufferSize()) < 0) {
		path.UnlockBuffer();
		return B_BAD_ADDRESS;
	}
	path.UnlockBuffer();

	// Only absolute paths, because the table is consulted against absolute
	// paths and a relative entry would silently never match.
	if (path.Path()[0] != '/')
		return B_BAD_VALUE;

	char permissions[8];
	if (user_strlcpy(permissions, userPermissions, sizeof(permissions)) < 0)
		return B_BAD_ADDRESS;

	uint32 requested;
	status_t status = unveil_parse_permissions(permissions, &requested);
	if (status != B_OK)
		return status;

	team_sandbox* sandbox = get_or_create_team_sandbox(team);
	if (sandbox == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(sandbox->lock);

	if (sandbox->unveil_locked)
		return B_NOT_ALLOWED;

	// Re-unveiling a path may only take permissions away. Widening one would
	// let a process that had already given up a directory take it back.
	for (size_t i = 0; i < sandbox->unveil_count; i++) {
		if (strcmp(sandbox->unveil_entries[i].path, path.Path()) != 0)
			continue;

		if ((requested & ~sandbox->unveil_entries[i].permissions) != 0)
			return B_NOT_ALLOWED;

		sandbox->unveil_entries[i].permissions = requested;
		return B_OK;
	}

	if (sandbox->unveil_count >= UNVEIL_MAX_ENTRIES)
		return B_NO_MEMORY;

	unveil_entry* entries = (unveil_entry*)realloc(sandbox->unveil_entries,
		sizeof(unveil_entry) * (sandbox->unveil_count + 1));
	if (entries == NULL)
		return B_NO_MEMORY;
	sandbox->unveil_entries = entries;

	unveil_entry& entry = entries[sandbox->unveil_count];
	entry.path = strdup(path.Path());
	if (entry.path == NULL)
		return B_NO_MEMORY;

	entry.path_length = strlen(entry.path);
	entry.permissions = requested;
	sandbox->unveil_count++;

	locker.Unlock();

	mark_team_sandboxed(team);
	return B_OK;
}


/*!	Resolves a syscall name to the index this build gave it.

	Without this a BPF policy cannot be written at all. The format compares
	against syscall_filter_data::nr, which carries a Haiku index, and those
	indices are generated per build and move whenever a call is added -- so a
	policy has to ask at run time rather than carry numbers of its own.

	Deliberately unrestricted. The names are already in every kernel image and
	knowing one tells a caller nothing it could not read out of the binary.
*/
status_t
_user_syscall_index(const char* userName, int32* _userIndex)
{
	if (userName == NULL || _userIndex == NULL)
		return B_BAD_VALUE;
	if (!IS_USER_ADDRESS(userName) || !IS_USER_ADDRESS(_userIndex))
		return B_BAD_ADDRESS;

	char name[128];
	if (user_strlcpy(name, userName, sizeof(name)) < 0)
		return B_BAD_ADDRESS;

	for (int i = 0; i < kSyscallCount; i++) {
		if (strcmp(kExtendedSyscallInfos[i].name, name) != 0)
			continue;

		const int32 index = i;
		if (user_memcpy(_userIndex, &index, sizeof(index)) != B_OK)
			return B_BAD_ADDRESS;

		return B_OK;
	}

	return B_NAME_NOT_FOUND;
}


status_t
_user_set_syscall_filter(const syscall_filter_program* userProgram,
	uint32 flags)
{
	if (userProgram == NULL || !IS_USER_ADDRESS(userProgram))
		return B_BAD_ADDRESS;

	syscall_filter_program program;
	if (user_memcpy(&program, userProgram, sizeof(program)) != B_OK)
		return B_BAD_ADDRESS;

	if (program.length == 0 || program.length > SYSCALL_FILTER_MAX_INSNS)
		return B_BAD_VALUE;
	if (program.instructions == NULL || !IS_USER_ADDRESS(program.instructions))
		return B_BAD_ADDRESS;

	const size_t size = sizeof(syscall_filter_insn) * program.length;
	syscall_filter_insn* instructions = (syscall_filter_insn*)malloc(size);
	if (instructions == NULL)
		return B_NO_MEMORY;
	MemoryDeleter instructionsDeleter(instructions);

	if (user_memcpy(instructions, program.instructions, size) != B_OK)
		return B_BAD_ADDRESS;

	// Verify the copy the kernel now owns, not the one in user memory, which
	// another thread of the same team could still be rewriting.
	status_t status = syscall_filter_validate(instructions, program.length);
	if (status != B_OK)
		return status;

	filter_entry* filter = (filter_entry*)malloc(sizeof(filter_entry));
	if (filter == NULL)
		return B_NO_MEMORY;

	filter->length = program.length;
	filter->flags = flags;
	filter->instructions = instructions;
	instructionsDeleter.Detach();

	Team* team = thread_get_current_thread()->team;
	team_sandbox* sandbox = get_or_create_team_sandbox(team);
	if (sandbox == NULL) {
		free(filter->instructions);
		free(filter);
		return B_NO_MEMORY;
	}

	MutexLocker locker(sandbox->lock);
	filter->next = sandbox->filters;
	sandbox->filters = filter;
	locker.Unlock();

	mark_team_sandboxed(team);
	return B_OK;
}
