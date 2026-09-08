/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_PLEDGE_DEFS_H
#define _SYSTEM_PLEDGE_DEFS_H


#include <SupportDefs.h>


/*!	Capability-based process confinement, after OpenBSD's pledge and unveil.

	A process states what it intends to do (pledge) and which parts of the
	file system it intends to reach (unveil), and the kernel holds it to that
	for the rest of its life. The point is that the statement is made while
	the process is still trustworthy: once it has parsed something hostile,
	the restrictions are already in place and cannot be lifted.

	Promises are named capabilities rather than syscall numbers. This is not
	only friendlier to write: Haiku's syscall indices are generated per build
	and shift whenever a call is added, so a policy written against numbers
	would silently start filtering the wrong calls after a rebuild. Names are
	stable, and the number they resolve to is looked up at build time.

	The promise set is deliberately not OpenBSD's. Haiku's system call surface
	is largely the BeAPI, so confining a Haiku program requires capabilities
	that OpenBSD has no equivalent for -- ports and areas for messaging,
	attributes, indices and live queries for BFS, node monitoring, and the
	app_server connection every GUI application depends on. Reusing OpenBSD's
	list unchanged would leave those calls either wholly unrestricted or
	wholly unusable.

	Restrictions may only ever be narrowed. A pledge cannot add a promise the
	process does not already hold, and unveil cannot widen access to a path,
	so neither call can be used to escape a confinement already in place. Both
	are inherited across thread creation and exec.
*/


/* --- Promises ---------------------------------------------------------- */

/*!	Always permitted: exit, and the calls needed to report failure.

	A confined process must always be able to die cleanly, and to find out
	that it has been denied something, or it could not react to the denial.
*/
#define PLEDGE_ALWAYS		0x0000000000000001ULL

/*!	Basic runtime services.

	Wider than OpenBSD's promise of the same name, of necessity. On Haiku the
	heap is built from areas and libroot's locks are semaphores, so a process
	that could not create either could not get as far as main(). Area
	creation under this promise is anonymous memory only; mapping a file
	still requires the corresponding path promise.
*/
#define PLEDGE_STDIO		0x0000000000000002ULL

/* File system access, by intent rather than by call. */
#define PLEDGE_RPATH		0x0000000000000004ULL	/* read paths */
#define PLEDGE_WPATH		0x0000000000000008ULL	/* write paths */
#define PLEDGE_CPATH		0x0000000000000010ULL	/* create/delete paths */
#define PLEDGE_DPATH		0x0000000000000020ULL	/* create device nodes */
#define PLEDGE_TMPPATH		0x0000000000000040ULL	/* /tmp only */
#define PLEDGE_FATTR		0x0000000000000080ULL	/* chmod/chown/utimes */
#define PLEDGE_FLOCK		0x0000000000000100ULL	/* lock files */

/* Networking. */
#define PLEDGE_INET			0x0000000000000200ULL	/* IPv4/IPv6 sockets */
#define PLEDGE_UNIX			0x0000000000000400ULL	/* local sockets */
#define PLEDGE_DNS			0x0000000000000800ULL	/* resolver access */
#define PLEDGE_SENDFD		0x0000000000001000ULL	/* pass descriptors */
#define PLEDGE_RECVFD		0x0000000000002000ULL	/* receive descriptors */

/* Processes, threads and images. */
#define PLEDGE_PROC			0x0000000000004000ULL	/* fork, signal, wait */
#define PLEDGE_EXEC			0x0000000000008000ULL	/* replace the image */
#define PLEDGE_THREAD		0x0000000000010000ULL	/* spawn threads */
#define PLEDGE_ID			0x0000000000020000ULL	/* change credentials */
#define PLEDGE_IMAGE		0x0000000000040000ULL	/* load add-ons */

/* System information. */
#define PLEDGE_PS			0x0000000000080000ULL	/* inspect other teams */
#define PLEDGE_VMINFO		0x0000000000100000ULL	/* memory statistics */
#define PLEDGE_GETPW		0x0000000000200000ULL	/* user database */
#define PLEDGE_SETTIME		0x0000000000400000ULL	/* set the clock */
#define PLEDGE_TTY			0x0000000000800000ULL	/* terminal control */

/* --- Haiku-specific ---------------------------------------------------- */

/*!	Port messaging: the transport under BMessage, and so under every
	connection to a system service. */
#define PLEDGE_PORT			0x0000000001000000ULL

/*!	Shared and cloned areas, as distinct from the anonymous memory that
	PLEDGE_STDIO already allows. Cloning another team's area is the way
	memory is shared on Haiku, and is a capability in its own right. */
#define PLEDGE_AREA			0x0000000002000000ULL

/*!	A connection to the app_server: ports and areas together, since the
	protocol needs both for the client-side drawing buffers. */
#define PLEDGE_APP			0x0000000004000000ULL

/* BFS features that have no POSIX equivalent. */
#define PLEDGE_ATTR			0x0000000008000000ULL	/* extended attributes */
#define PLEDGE_INDEX		0x0000000010000000ULL	/* filesystem indices */
#define PLEDGE_QUERY		0x0000000020000000ULL	/* live queries */
#define PLEDGE_NODE_MONITOR	0x0000000040000000ULL	/* node monitoring */

/* Rarely wanted, and dangerous to leave open. */
#define PLEDGE_SEM			0x0000000080000000ULL	/* named/XSI semaphores */
#define PLEDGE_DISK			0x0000000100000000ULL	/* disk device manager */
#define PLEDGE_MOUNT		0x0000000200000000ULL	/* mount/unmount */
#define PLEDGE_DEBUG		0x0000000400000000ULL	/* debug other teams */
#define PLEDGE_SETTINGS		0x0000000800000000ULL	/* kernel settings */

#define PLEDGE_ALL_PROMISES	0x0000000fffffffffULL


/* --- unveil ------------------------------------------------------------ */

#define UNVEIL_READ			0x01	/* "r" */
#define UNVEIL_WRITE		0x02	/* "w" */
#define UNVEIL_EXEC			0x04	/* "x" */
#define UNVEIL_CREATE		0x08	/* "c" */

#define UNVEIL_MAX_ENTRIES	128


/*!	An unveiled path and what may be done with it.

	The path is held as a pointer rather than a fixed array: a team may unveil
	up to UNVEIL_MAX_ENTRIES paths, and reserving B_PATH_NAME_LENGTH for each
	would cost far more per team than the handful of short paths a real policy
	uses. Paths are absolute and canonical by the time they are stored.
*/
typedef struct unveil_entry {
	char*	path;
	size_t	path_length;
	uint32	permissions;
} unveil_entry;


/*!	What the kernel does when a process breaks its promise.

	Killing is the default, and is deliberate: a process that has just done
	something it promised not to is most likely already under someone else's
	control, and returning an error would let that continue with the failure
	merely reported. Returning ENOSYS instead is available for bringing an
	existing program under a policy, where the aim is to discover what it
	actually needs without it dying at the first surprise.
*/
enum pledge_violation_action {
	PLEDGE_KILL		= 0,	/* kill the team (default) */
	PLEDGE_ERROR	= 1,	/* fail the call with ENOSYS */
};


#define PLEDGE_FLAG_ERROR		0x01	/* use PLEDGE_ERROR */
#define PLEDGE_FLAG_LOG			0x02	/* log every violation */


/*!	Maps a promise name to its bit. */
typedef struct pledge_promise_info {
	const char*	name;
	uint64		bit;
} pledge_promise_info;


#ifdef __cplusplus
extern "C" {
#endif

extern const pledge_promise_info kPledgePromises[];
extern const size_t kPledgePromiseCount;

status_t	pledge_parse_promises(const char* promises, uint64* _mask);
status_t	unveil_parse_permissions(const char* permissions, uint32* _flags);

bool		unveil_check_access(const unveil_entry* entries, size_t count,
				const char* path, uint32 requested);

#ifdef __cplusplus
}
#endif


#endif	/* _SYSTEM_PLEDGE_DEFS_H */
