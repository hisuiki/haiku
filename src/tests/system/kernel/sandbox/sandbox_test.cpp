/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Sandbox.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


/*!	Exercises pledge, unveil and the syscall filter against a live kernel.

	Every case that expects to be refused is run in a child, because the
	default action for a broken promise is to kill the team: a test that
	checked it in-process would take the rest of the suite with it. The parent
	looks at how the child died, which is also the only way to tell a kill
	from a mere error return.
*/


static int sFailures = 0;


static void
report(const char* name, bool passed, const char* detail)
{
	printf("%-46s %s", name, passed ? "ok" : "FAILED");
	if (detail != NULL && detail[0] != '\0')
		printf("  (%s)", detail);
	printf("\n");

	if (!passed)
		sFailures++;
}


/*!	Runs \a test in a child and reports how it ended.

	\return The child's exit code, or -1 if it was killed by a signal.
*/
static int
run_child(void (*test)(void))
{
	pid_t child = fork();
	if (child < 0) {
		report("fork", false, strerror(errno));
		return -2;
	}

	if (child == 0) {
		test();
		exit(0);
	}

	int status = 0;
	if (waitpid(child, &status, 0) != child)
		return -2;

	if (WIFSIGNALED(status))
		return -1;

	return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}


// #pragma mark - pledge


static void
child_pledge_allows_promised(void)
{
	if (pledge("stdio rpath", 0) != B_OK)
		exit(2);

	int fd = open("/boot/system/lib", O_RDONLY);
	if (fd < 0)
		exit(3);
	close(fd);

	exit(0);
}


static void
child_pledge_kills_on_violation(void)
{
	if (pledge("stdio", 0) != B_OK)
		exit(2);

	// rpath was not promised, so this must not come back at all.
	open("/boot/system/lib", O_RDONLY);
	exit(4);
}


static void
child_pledge_error_mode(void)
{
	if (pledge("stdio", PLEDGE_FLAG_ERROR) != B_OK)
		exit(2);

	if (open("/boot/system/lib", O_RDONLY) >= 0)
		exit(4);

	exit(0);
}


static void
child_pledge_cannot_widen(void)
{
	if (pledge("stdio", 0) != B_OK)
		exit(2);

	// Asking for rpath back must be refused rather than granted.
	if (pledge("stdio rpath", 0) != B_NOT_ALLOWED)
		exit(4);

	exit(0);
}


static void
child_pledge_rejects_unknown_promise(void)
{
	if (pledge("stdio nosuchpromise", 0) != B_BAD_VALUE)
		exit(4);

	exit(0);
}


static void
child_pledge_survives_fork(void)
{
	if (pledge("stdio proc", PLEDGE_FLAG_ERROR) != B_OK)
		exit(2);

	pid_t child = fork();
	if (child < 0)
		exit(3);

	if (child == 0) {
		// The child inherited the promise set, so rpath is still missing.
		exit(open("/boot/system/lib", O_RDONLY) >= 0 ? 1 : 0);
	}

	int status = 0;
	waitpid(child, &status, 0);
	exit(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 4);
}


// #pragma mark - unveil


static void
child_unveil_allows_unveiled(void)
{
	if (unveil("/boot/system/lib", "r") != B_OK)
		exit(2);
	if (pledge("stdio rpath", PLEDGE_FLAG_ERROR) != B_OK)
		exit(3);

	int fd = open("/boot/system/lib", O_RDONLY);
	if (fd < 0)
		exit(4);
	close(fd);

	exit(0);
}


static void
child_unveil_hides_everything_else(void)
{
	if (unveil("/boot/system/lib", "r") != B_OK)
		exit(2);
	if (pledge("stdio rpath", PLEDGE_FLAG_ERROR) != B_OK)
		exit(3);

	// Not unveiled, so it must be invisible even though rpath was promised.
	if (open("/boot/system/bin", O_RDONLY) >= 0)
		exit(4);

	exit(0);
}


static void
child_unveil_cannot_widen(void)
{
	if (unveil("/boot/system/lib", "r") != B_OK)
		exit(2);

	if (unveil("/boot/system/lib", "rw") != B_NOT_ALLOWED)
		exit(4);

	exit(0);
}


static void
child_unveil_lock_holds(void)
{
	if (unveil("/boot/system/lib", "r") != B_OK)
		exit(2);
	if (unveil_lock() != B_OK)
		exit(3);

	if (unveil("/boot/system/bin", "r") != B_NOT_ALLOWED)
		exit(4);

	exit(0);
}


static void
child_unveil_rejects_relative_path(void)
{
	if (unveil("lib", "r") != B_BAD_VALUE)
		exit(4);

	exit(0);
}


// #pragma mark - syscall filter


/*!	A program that allows everything.

	The smallest thing the verifier should accept, and the check that an
	installed filter does not break a process that stays inside it.
*/
static void
child_filter_allow_all(void)
{
	syscall_filter_insn instructions[] = {
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	syscall_filter_program program = { 1, instructions };

	if (set_syscall_filter(&program, 0) != B_OK)
		exit(2);

	int fd = open("/boot/system/lib", O_RDONLY);
	if (fd < 0)
		exit(3);
	close(fd);

	exit(0);
}


/*!	A program that refuses one call and allows the rest.

	The shape every real policy has. It cannot refuse everything: the process
	has to survive to report what happened, and exiting is a system call too.

	The number to compare against is asked for rather than written down --
	Haiku's syscall indices are generated per build and move whenever a call
	is added, so a policy that carried its own numbers would quietly start
	filtering something else after a rebuild.
*/
static void
child_filter_errno(void)
{
	int32 openIndex = -1;
	if (find_syscall_index("_kern_open", &openIndex) != B_OK)
		exit(2);

	syscall_filter_insn instructions[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0,
			(uint32)offsetof(syscall_filter_data, nr) },
		{ SYSCALL_FILTER_JMP | SYSCALL_FILTER_JEQ | SYSCALL_FILTER_K, 0, 1,
			(uint32)openIndex },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_ENOSYS },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	syscall_filter_program program = { 4, instructions };

	if (set_syscall_filter(&program, 0) != B_OK)
		exit(3);

	if (open("/boot/system/lib", O_RDONLY) >= 0)
		exit(4);
	if (errno != ENOSYS)
		exit(5);

	// Everything the filter did not name still works, which is what makes it
	// possible to see the refusal at all.
	if (close(0) != 0 && errno == ENOSYS)
		exit(6);

	exit(0);
}


/*!	The name lookup a policy is written against. */
static void
child_filter_index_lookup(void)
{
	int32 index = -1;
	if (find_syscall_index("_kern_open", &index) != B_OK || index < 0)
		exit(4);

	int32 other = -1;
	if (find_syscall_index("_kern_close", &other) != B_OK || other == index)
		exit(5);

	if (find_syscall_index("_kern_no_such_call", &index) != B_NAME_NOT_FOUND)
		exit(6);

	exit(0);
}


/*!	A backwards jump, which the verifier has to reject or the kernel could
	be made to loop inside a syscall. */
static void
child_filter_rejects_backward_jump(void)
{
	syscall_filter_insn instructions[] = {
		{ SYSCALL_FILTER_JMP | SYSCALL_FILTER_JA, 0, 0, 0xffffffff },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	syscall_filter_program program = { 2, instructions };

	if (set_syscall_filter(&program, 0) != B_BAD_VALUE)
		exit(4);

	exit(0);
}


/*!	A load past the end of the record, which must not be allowed to read
	whatever the kernel keeps after it. */
static void
child_filter_rejects_out_of_range_load(void)
{
	syscall_filter_insn instructions[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0,
			0x10000 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	syscall_filter_program program = { 2, instructions };

	if (set_syscall_filter(&program, 0) != B_BAD_VALUE)
		exit(4);

	exit(0);
}


/*!	A program that does not end in a return, so evaluation could fall off
	the end of it. */
static void
child_filter_rejects_missing_return(void)
{
	syscall_filter_insn instructions[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0, 0 },
	};
	syscall_filter_program program = { 1, instructions };

	if (set_syscall_filter(&program, 0) != B_BAD_VALUE)
		exit(4);

	exit(0);
}


// #pragma mark -


struct test_case {
	const char*	name;
	void		(*function)(void);
	int			expected;	// exit code, or -1 for "killed by a signal"
};


int
main()
{
	printf("promises available:");
	for (size_t i = 0; i < kPledgePromiseCount; i++)
		printf(" %s", kPledgePromises[i].name);
	printf("\n\n");

	const test_case tests[] = {
		{ "pledge allows a promised call",
			child_pledge_allows_promised, 0 },
		{ "pledge kills on a broken promise",
			child_pledge_kills_on_violation, -1 },
		{ "pledge can fail instead of killing",
			child_pledge_error_mode, 0 },
		{ "pledge cannot take a promise back",
			child_pledge_cannot_widen, 0 },
		{ "pledge rejects an unknown promise",
			child_pledge_rejects_unknown_promise, 0 },
		{ "pledge is inherited across fork",
			child_pledge_survives_fork, 0 },
		{ "unveil allows an unveiled path",
			child_unveil_allows_unveiled, 0 },
		{ "unveil hides everything else",
			child_unveil_hides_everything_else, 0 },
		{ "unveil cannot widen an entry",
			child_unveil_cannot_widen, 0 },
		{ "unveil lock holds",
			child_unveil_lock_holds, 0 },
		{ "unveil rejects a relative path",
			child_unveil_rejects_relative_path, 0 },
		{ "filter allowing everything changes nothing",
			child_filter_allow_all, 0 },
		{ "filter can fail one call and allow the rest",
			child_filter_errno, 0 },
		{ "a syscall name resolves to an index",
			child_filter_index_lookup, 0 },
		{ "filter verifier rejects a backward jump",
			child_filter_rejects_backward_jump, 0 },
		{ "filter verifier rejects a load past the end",
			child_filter_rejects_out_of_range_load, 0 },
		{ "filter verifier rejects a missing return",
			child_filter_rejects_missing_return, 0 },
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		const int result = run_child(tests[i].function);
		char detail[64];

		detail[0] = '\0';
		if (result != tests[i].expected) {
			snprintf(detail, sizeof(detail), "expected %d, got %d",
				tests[i].expected, result);
		}

		report(tests[i].name, result == tests[i].expected, detail);
	}

	printf("\n%d failure(s)\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
