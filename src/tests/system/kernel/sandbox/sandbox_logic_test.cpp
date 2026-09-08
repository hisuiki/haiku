// Host-side exercise of the parts of the confinement logic that touch no
// kernel state, so the verifier and the path matcher can be checked without
// booting anything.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <pledge_defs.h>
#include <syscall_filter_defs.h>

static int sFailures = 0;

static void
check(const char* what, bool ok)
{
	printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		sFailures++;
}

static unveil_entry
entry(char* path, uint32 permissions)
{
	unveil_entry e;
	e.path = path;
	e.path_length = strlen(path);
	e.permissions = permissions;
	return e;
}

int
main()
{
	uint64 mask = 0;

	check("empty promise list keeps only the always-on bit",
		pledge_parse_promises("", &mask) == B_OK && mask == PLEDGE_ALWAYS);
	check("a single promise parses",
		pledge_parse_promises("stdio", &mask) == B_OK
			&& mask == (PLEDGE_ALWAYS | PLEDGE_STDIO));
	check("several promises parse",
		pledge_parse_promises("stdio rpath port", &mask) == B_OK
			&& mask == (PLEDGE_ALWAYS | PLEDGE_STDIO | PLEDGE_RPATH
				| PLEDGE_PORT));
	check("extra whitespace is tolerated",
		pledge_parse_promises("  stdio\trpath  ", &mask) == B_OK
			&& mask == (PLEDGE_ALWAYS | PLEDGE_STDIO | PLEDGE_RPATH));
	check("an unknown promise is refused",
		pledge_parse_promises("stdio bogus", &mask) == B_BAD_VALUE);
	check("a name that merely starts with a promise is refused",
		pledge_parse_promises("rpathx", &mask) == B_BAD_VALUE);
	check("NULL means leave the set alone",
		pledge_parse_promises(NULL, &mask) == B_OK
			&& mask == PLEDGE_ALL_PROMISES);

	uint32 flags = 0;
	check("permission letters parse",
		unveil_parse_permissions("rwxc", &flags) == B_OK
			&& flags == (UNVEIL_READ | UNVEIL_WRITE | UNVEIL_EXEC
				| UNVEIL_CREATE));
	check("an empty permission string is no access",
		unveil_parse_permissions("", &flags) == B_OK && flags == 0);
	check("an unknown permission letter is refused",
		unveil_parse_permissions("rz", &flags) == B_BAD_VALUE);

	char etc[] = "/etc";
	char etcSsl[] = "/etc/ssl";
	char root[] = "/";

	unveil_entry table[2] = {
		entry(etc, UNVEIL_READ),
		entry(etcSsl, 0),
	};

	check("an empty table permits everything",
		unveil_check_access(table, 0, "/anything", UNVEIL_READ));
	check("an unveiled path is readable",
		unveil_check_access(table, 2, "/etc/passwd", UNVEIL_READ));
	check("the unveiled path itself is readable",
		unveil_check_access(table, 2, "/etc", UNVEIL_READ));
	check("a longer entry overrides a shorter one",
		!unveil_check_access(table, 2, "/etc/ssl/private", UNVEIL_READ));
	check("access beyond what was granted is refused",
		!unveil_check_access(table, 2, "/etc/passwd", UNVEIL_WRITE));
	check("a path outside the table is hidden",
		!unveil_check_access(table, 2, "/home/user", UNVEIL_READ));
	check("a sibling sharing a textual prefix is not covered",
		!unveil_check_access(table, 2, "/etcfoo", UNVEIL_READ));

	unveil_entry rootTable[1] = { entry(root, UNVEIL_READ) };
	check("the root entry covers everything",
		unveil_check_access(rootTable, 1, "/home/user", UNVEIL_READ));

	// The verifier.
	syscall_filter_insn ret[] = {
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a lone return is accepted",
		syscall_filter_validate(ret, 1) == B_OK);
	check("an empty program is refused",
		syscall_filter_validate(ret, 0) == B_BAD_VALUE);

	syscall_filter_insn noReturn[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0, 0 },
	};
	check("a program that can fall off the end is refused",
		syscall_filter_validate(noReturn, 1) == B_BAD_VALUE);

	syscall_filter_insn backward[] = {
		{ SYSCALL_FILTER_JMP | SYSCALL_FILTER_JA, 0, 0, 0xffffffff },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a backward jump is refused",
		syscall_filter_validate(backward, 2) == B_BAD_VALUE);

	syscall_filter_insn farLoad[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0,
			0x10000 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a load past the end of the record is refused",
		syscall_filter_validate(farLoad, 2) == B_BAD_VALUE);

	syscall_filter_insn wrapLoad[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0,
			0xfffffffc },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a load whose bound would wrap is refused",
		syscall_filter_validate(wrapLoad, 2) == B_BAD_VALUE);

	syscall_filter_insn unaligned[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0, 2 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("an unaligned load is refused",
		syscall_filter_validate(unaligned, 2) == B_BAD_VALUE);

	syscall_filter_insn divideByZero[] = {
		{ SYSCALL_FILTER_ALU | SYSCALL_FILTER_DIV | SYSCALL_FILTER_K, 0, 0, 0 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a constant division by zero is refused",
		syscall_filter_validate(divideByZero, 2) == B_BAD_VALUE);

	syscall_filter_insn farScratch[] = {
		{ SYSCALL_FILTER_ST, 0, 0, SYSCALL_FILTER_MEMWORDS },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("a store past the scratch memory is refused",
		syscall_filter_validate(farScratch, 2) == B_BAD_VALUE);

	// The interpreter: the shape every real policy has, a comparison on the
	// syscall number followed by two different answers.
	syscall_filter_insn policy[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0, 0 },
		{ SYSCALL_FILTER_JMP | SYSCALL_FILTER_JEQ | SYSCALL_FILTER_K, 0, 1,
			42 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_KILL_PROCESS },
	};
	check("the policy shape is accepted",
		syscall_filter_validate(policy, 4) == B_OK);

	syscall_filter_data data;
	memset(&data, 0, sizeof(data));

	data.nr = 42;
	check("the matching syscall is allowed",
		syscall_filter_run(policy, 4, &data) == SYSCALL_FILTER_RET_ALLOW);
	data.nr = 43;
	check("every other syscall is killed",
		syscall_filter_run(policy, 4, &data)
			== SYSCALL_FILTER_RET_KILL_PROCESS);

	// Argument inspection, which is what a policy needs to allow one flag
	// value of a call and refuse the rest.
	syscall_filter_insn argument[] = {
		{ SYSCALL_FILTER_LD | SYSCALL_FILTER_W | SYSCALL_FILTER_ABS, 0, 0,
			(uint32)offsetof(syscall_filter_data, args[1]) },
		{ SYSCALL_FILTER_JMP | SYSCALL_FILTER_JSET | SYSCALL_FILTER_K, 0, 1,
			0x4 },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_EPERM },
		{ SYSCALL_FILTER_RET | SYSCALL_FILTER_K, 0, 0,
			SYSCALL_FILTER_RET_ALLOW },
	};
	check("argument inspection is accepted",
		syscall_filter_validate(argument, 4) == B_OK);

	data.args[1] = 0x6;
	check("the refused flag value returns an errno",
		syscall_filter_run(argument, 4, &data)
			== (SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_EPERM));
	data.args[1] = 0x1;
	check("other flag values are allowed",
		syscall_filter_run(argument, 4, &data) == SYSCALL_FILTER_RET_ALLOW);

	// Killing the process is 0x80000000, which is the largest action there is
	// read as unsigned and the smallest read as signed. Both orders are
	// checked, because a comparison that got the signedness wrong would still
	// pass one of them.
	check("killing the process beats allowing",
		syscall_filter_combine(SYSCALL_FILTER_RET_ALLOW,
			SYSCALL_FILTER_RET_KILL_PROCESS)
				== SYSCALL_FILTER_RET_KILL_PROCESS);
	check("killing the process beats allowing, either way round",
		syscall_filter_combine(SYSCALL_FILTER_RET_KILL_PROCESS,
			SYSCALL_FILTER_RET_ALLOW)
				== SYSCALL_FILTER_RET_KILL_PROCESS);
	check("killing the thread beats allowing",
		syscall_filter_combine(SYSCALL_FILTER_RET_ALLOW,
			SYSCALL_FILTER_RET_KILL_THREAD)
				== SYSCALL_FILTER_RET_KILL_THREAD);
	check("killing the process beats killing the thread",
		syscall_filter_combine(SYSCALL_FILTER_RET_KILL_THREAD,
			SYSCALL_FILTER_RET_KILL_PROCESS)
				== SYSCALL_FILTER_RET_KILL_PROCESS);
	check("an errno beats allowing",
		syscall_filter_combine(SYSCALL_FILTER_RET_ALLOW,
			SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_EPERM)
				== (SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_EPERM));
	check("a trap beats an errno",
		syscall_filter_combine(
			SYSCALL_FILTER_RET_ERRNO | SYSCALL_FILTER_ERRNO_EPERM,
			SYSCALL_FILTER_RET_TRAP) == SYSCALL_FILTER_RET_TRAP);
	check("logging still allows the call through",
		syscall_filter_combine(SYSCALL_FILTER_RET_ALLOW,
			SYSCALL_FILTER_RET_LOG) == SYSCALL_FILTER_RET_LOG);
	check("combining two allows still allows",
		syscall_filter_combine(SYSCALL_FILTER_RET_ALLOW,
			SYSCALL_FILTER_RET_ALLOW) == SYSCALL_FILTER_RET_ALLOW);

	printf("\n%d failure(s)\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
