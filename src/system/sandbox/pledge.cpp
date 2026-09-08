/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <pledge_defs.h>

#include <string.h>


/*!	Promise parsing and unveil path matching.

	The logic here is deliberately free of kernel state so that it can be
	exercised directly by tests: everything takes its inputs as arguments and
	returns a decision. The parts that must touch a team -- installing a
	promise set, walking the unveil table during path resolution -- build on
	these functions rather than reimplementing them.
*/


const pledge_promise_info kPledgePromises[] = {
	{ "stdio",			PLEDGE_STDIO },
	{ "rpath",			PLEDGE_RPATH },
	{ "wpath",			PLEDGE_WPATH },
	{ "cpath",			PLEDGE_CPATH },
	{ "dpath",			PLEDGE_DPATH },
	{ "tmppath",		PLEDGE_TMPPATH },
	{ "fattr",			PLEDGE_FATTR },
	{ "flock",			PLEDGE_FLOCK },
	{ "inet",			PLEDGE_INET },
	{ "unix",			PLEDGE_UNIX },
	{ "dns",			PLEDGE_DNS },
	{ "sendfd",			PLEDGE_SENDFD },
	{ "recvfd",			PLEDGE_RECVFD },
	{ "proc",			PLEDGE_PROC },
	{ "exec",			PLEDGE_EXEC },
	{ "thread",			PLEDGE_THREAD },
	{ "id",				PLEDGE_ID },
	{ "image",			PLEDGE_IMAGE },
	{ "ps",				PLEDGE_PS },
	{ "vminfo",			PLEDGE_VMINFO },
	{ "getpw",			PLEDGE_GETPW },
	{ "settime",		PLEDGE_SETTIME },
	{ "tty",			PLEDGE_TTY },
	{ "port",			PLEDGE_PORT },
	{ "area",			PLEDGE_AREA },
	{ "app",			PLEDGE_APP },
	{ "attr",			PLEDGE_ATTR },
	{ "index",			PLEDGE_INDEX },
	{ "query",			PLEDGE_QUERY },
	{ "node_monitor",	PLEDGE_NODE_MONITOR },
	{ "sem",			PLEDGE_SEM },
	{ "disk",			PLEDGE_DISK },
	{ "mount",			PLEDGE_MOUNT },
	{ "debug",			PLEDGE_DEBUG },
	{ "settings",		PLEDGE_SETTINGS },
};

const size_t kPledgePromiseCount
	= sizeof(kPledgePromises) / sizeof(kPledgePromises[0]);


/*!	Turns a space-separated promise list into a bit mask.

	An unknown name is an error rather than something to ignore. A typo would
	otherwise silently drop a restriction the caller believed it had asked
	for, or -- worse, once names are added over time -- mean one thing on the
	kernel it was written against and another on a newer one.

	\param promises The list, or NULL to leave the current set untouched.
	\param _mask Filled in with the parsed mask.
	\return \c B_OK, or \c B_BAD_VALUE for an unknown name.
*/
status_t
pledge_parse_promises(const char* promises, uint64* _mask)
{
	if (_mask == NULL)
		return B_BAD_VALUE;

	// A caller that wants to change only its unveil state passes NULL here,
	// which is distinct from "" -- the empty string is the valid request to
	// give up every promise.
	if (promises == NULL) {
		*_mask = PLEDGE_ALL_PROMISES;
		return B_OK;
	}

	uint64 mask = PLEDGE_ALWAYS;
	const char* p = promises;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;

		const char* start = p;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;

		const size_t length = (size_t)(p - start);
		bool found = false;

		for (size_t i = 0; i < kPledgePromiseCount; i++) {
			// Compare the length too, or "rpath" would be accepted for a
			// request that actually read "rpathx".
			if (strlen(kPledgePromises[i].name) == length
				&& strncmp(kPledgePromises[i].name, start, length) == 0) {
				mask |= kPledgePromises[i].bit;
				found = true;
				break;
			}
		}

		if (!found)
			return B_BAD_VALUE;
	}

	*_mask = mask;
	return B_OK;
}


/*!	Parses an unveil permission string such as "rwc".

	\return \c B_OK, or \c B_BAD_VALUE for an unknown letter.
*/
status_t
unveil_parse_permissions(const char* permissions, uint32* _flags)
{
	if (permissions == NULL || _flags == NULL)
		return B_BAD_VALUE;

	uint32 flags = 0;

	for (const char* p = permissions; *p != '\0'; p++) {
		switch (*p) {
			case 'r':
				flags |= UNVEIL_READ;
				break;
			case 'w':
				flags |= UNVEIL_WRITE;
				break;
			case 'x':
				flags |= UNVEIL_EXEC;
				break;
			case 'c':
				flags |= UNVEIL_CREATE;
				break;
			default:
				return B_BAD_VALUE;
		}
	}

	*_flags = flags;
	return B_OK;
}


/*!	Tests whether \a path lies at or below \a prefix.

	Comparing the prefix bytes alone is not enough: that would make "/etc"
	cover "/etcpasswd", because the shorter string does prefix the longer
	one. A cover only counts when the next character of the path is a
	separator, so that the match falls on a component boundary.

	Both arguments must already be absolute and canonical. Resolving "..",
	symlinks and relative paths is the caller's job, and doing it before this
	point is what stops "/tmp/../etc/passwd" from being tested as though it
	were under /tmp.

	\return \c true if \a prefix is \a path or one of its ancestors.
*/
static bool
path_is_covered_by(const char* path, const char* prefix)
{
	if (path == NULL || prefix == NULL)
		return false;

	const size_t prefixLength = strlen(prefix);
	if (prefixLength == 0)
		return false;

	// The root covers everything, and has no boundary to check: the "/" it
	// ends with is the separator itself.
	if (prefixLength == 1 && prefix[0] == '/')
		return path[0] == '/';

	if (strncmp(path, prefix, prefixLength) != 0)
		return false;

	// Same path.
	if (path[prefixLength] == '\0')
		return true;

	// Genuinely below the prefix.
	if (path[prefixLength] == '/')
		return true;

	// Shares a textual prefix but not a component boundary: /etc vs /etcfoo.
	return false;
}


/*!	Finds the entry governing \a path.

	The most specific match wins, so that a narrow rule can be granted inside
	a broader one -- unveil("/etc", "r") together with unveil("/etc/ssl", "")
	has to leave /etc readable while shutting /etc/ssl off entirely, and that
	only works if the longer prefix is the one consulted.

	\return The governing entry, or NULL if no entry covers \a path.
*/
static const unveil_entry*
unveil_find_entry(const unveil_entry* entries, size_t count, const char* path)
{
	const unveil_entry* best = NULL;

	for (size_t i = 0; i < count; i++) {
		if (!path_is_covered_by(path, entries[i].path))
			continue;

		if (best == NULL || entries[i].path_length > best->path_length)
			best = &entries[i];
	}

	return best;
}


/*!	Decides whether \a path may be used in the way \a requested asks.

	\param entries The team's unveil table.
	\param count Its length; zero means unveil was never called, which leaves
		the file system visible and defers entirely to the promises.
	\param path An absolute, canonical path.
	\param requested The UNVEIL_* access being attempted.
	\return \c true if the access is permitted.
*/
bool
unveil_check_access(const unveil_entry* entries, size_t count,
	const char* path, uint32 requested)
{
	if (count == 0)
		return true;

	const unveil_entry* entry = unveil_find_entry(entries, count, path);

	// Once anything has been unveiled, everything else is hidden; that is
	// what makes unveil a whitelist rather than a set of exceptions.
	if (entry == NULL)
		return false;

	// An entry unveiled with "" hides a subtree that a broader entry would
	// otherwise have covered, so it has to deny rather than fall back.
	return (entry->permissions & requested) == requested;
}
