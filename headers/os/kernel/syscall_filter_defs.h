/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_SYSCALL_FILTER_DEFS_H
#define _SYSTEM_SYSCALL_FILTER_DEFS_H


#include <SupportDefs.h>


/*!	Classic BPF syscall filtering, in the Linux seccomp-BPF program format.

	Haiku has neither seccomp() nor the BSD confinement primitives: the
	compatibility layers under src/libs/compat shim FreeBSD and OpenBSD
	*network drivers* only, and provide nothing that can restrict what a
	compromised process is able to ask the kernel for. Sandboxes that already
	express their policy as classic BPF programs (Chromium's among them) thus
	have no engine to run them against.

	The program encoding, the instruction set, and the action values below
	deliberately match Linux so that existing policy *generators* can be
	reused unchanged. The data a program is evaluated against deliberately
	does not: syscall_filter_data::nr carries a Haiku syscall index, whose
	numbering is unrelated to Linux's and is generated per build. Policies
	must therefore be written against Haiku's syscall table; a program that
	assumes Linux syscall numbers will silently filter the wrong calls.

	A filter may only ever narrow what a team can do. Filters are additive
	and cannot be removed, and once installed they are inherited across both
	thread creation and exec, so a sandboxed process cannot escape by
	re-execing a fresh image.
*/


/* An instruction; layout-compatible with Linux's struct sock_filter. */
typedef struct syscall_filter_insn {
	uint16	code;			/* operation */
	uint8	jt;				/* jump if true, relative to the next insn */
	uint8	jf;				/* jump if false, relative to the next insn */
	uint32	k;				/* immediate operand */
} syscall_filter_insn;


/* A program; layout-compatible with Linux's struct sock_fprog. */
typedef struct syscall_filter_program {
	uint16						length;		/* instruction count */
	syscall_filter_insn*		instructions;
} syscall_filter_program;


/*!	The record a filter program is evaluated against.

	Layout-compatible with Linux's struct seccomp_data so that the byte
	offsets emitted by existing policy generators resolve to the intended
	fields. Loads are only permitted inside this structure.
*/
typedef struct syscall_filter_data {
	int32	nr;						/* Haiku syscall index */
	uint32	arch;					/* one of SYSCALL_FILTER_ARCH_* */
	uint64	instruction_pointer;	/* userland IP of the calling insn */
	uint64	args[6];				/* syscall arguments */
} syscall_filter_data;


/* Architecture tokens, so a program can refuse to run on an ABI it was not
   written for. These are Haiku's own; they intentionally do not collide with
   the Linux AUDIT_ARCH_* values, because the syscall numbering differs. */
#define SYSCALL_FILTER_ARCH_X86_64		0x4001
#define SYSCALL_FILTER_ARCH_X86			0x4002
#define SYSCALL_FILTER_ARCH_ARM64		0x4003
#define SYSCALL_FILTER_ARCH_ARM			0x4004
#define SYSCALL_FILTER_ARCH_RISCV64		0x4005


/* Actions. The *lowest* action returned by any installed filter wins, so
   adding a filter can only ever restrict further. */
#define SYSCALL_FILTER_RET_KILL_PROCESS	0x80000000	/* kill the whole team */
#define SYSCALL_FILTER_RET_KILL_THREAD	0x00000000	/* kill the caller */
#define SYSCALL_FILTER_RET_TRAP			0x00030000	/* raise SIGSYS */
#define SYSCALL_FILTER_RET_ERRNO		0x00050000	/* fail with data as error */
#define SYSCALL_FILTER_RET_TRACE		0x7ff00000	/* notify the debugger */
#define SYSCALL_FILTER_RET_LOG			0x7ffc0000	/* allow, but log */
#define SYSCALL_FILTER_RET_ALLOW		0x7fff0000	/* allow */

#define SYSCALL_FILTER_RET_ACTION_MASK	0xffff0000
#define SYSCALL_FILTER_RET_DATA_MASK	0x0000ffff

/* The data a SYSCALL_FILTER_RET_ERRNO action carries.

   These are the *Linux* errno numbers, not Haiku's. Haiku has no small
   positive errno values at all -- its E* constants are the B_* status codes,
   which are large and negative and would not fit the 16-bit field the format
   provides. Keeping Linux's numbering is what lets an existing policy be
   loaded unchanged; the kernel translates to the corresponding status_t
   before the refused call returns, and falls back to B_NOT_ALLOWED for a
   number it does not recognise. */
#define SYSCALL_FILTER_ERRNO_EPERM		1
#define SYSCALL_FILTER_ERRNO_ENOENT		2
#define SYSCALL_FILTER_ERRNO_EBADF		9
#define SYSCALL_FILTER_ERRNO_EAGAIN		11
#define SYSCALL_FILTER_ERRNO_ENOMEM		12
#define SYSCALL_FILTER_ERRNO_EACCES		13
#define SYSCALL_FILTER_ERRNO_EFAULT		14
#define SYSCALL_FILTER_ERRNO_EINVAL		22
#define SYSCALL_FILTER_ERRNO_ENOSYS		38

/* Flags for _kern_set_syscall_filter(). */
#define SYSCALL_FILTER_FLAG_LOG			0x01	/* log every denial */

#define SYSCALL_FILTER_MAX_INSNS		4096
#define SYSCALL_FILTER_MEMWORDS			16


/* Instruction classes. */
#define SYSCALL_FILTER_CLASS(code)	((code) & 0x07)
#define SYSCALL_FILTER_LD			0x00
#define SYSCALL_FILTER_LDX			0x01
#define SYSCALL_FILTER_ST			0x02
#define SYSCALL_FILTER_STX			0x03
#define SYSCALL_FILTER_ALU			0x04
#define SYSCALL_FILTER_JMP			0x05
#define SYSCALL_FILTER_RET			0x06
#define SYSCALL_FILTER_MISC			0x07

/* Operand widths. */
#define SYSCALL_FILTER_SIZE(code)	((code) & 0x18)
#define SYSCALL_FILTER_W			0x00
#define SYSCALL_FILTER_H			0x08
#define SYSCALL_FILTER_B			0x10

/* Addressing modes. */
#define SYSCALL_FILTER_MODE(code)	((code) & 0xe0)
#define SYSCALL_FILTER_IMM			0x00
#define SYSCALL_FILTER_ABS			0x20
#define SYSCALL_FILTER_IND			0x40
#define SYSCALL_FILTER_MEM			0x60
#define SYSCALL_FILTER_LEN			0x80
#define SYSCALL_FILTER_MSH			0xa0

/* ALU and jump operations. */
#define SYSCALL_FILTER_OP(code)		((code) & 0xf0)
#define SYSCALL_FILTER_ADD			0x00
#define SYSCALL_FILTER_SUB			0x10
#define SYSCALL_FILTER_MUL			0x20
#define SYSCALL_FILTER_DIV			0x30
#define SYSCALL_FILTER_OR			0x40
#define SYSCALL_FILTER_AND			0x50
#define SYSCALL_FILTER_LSH			0x60
#define SYSCALL_FILTER_RSH			0x70
#define SYSCALL_FILTER_NEG			0x80
#define SYSCALL_FILTER_MOD			0x90
#define SYSCALL_FILTER_XOR			0xa0

#define SYSCALL_FILTER_JA			0x00
#define SYSCALL_FILTER_JEQ			0x10
#define SYSCALL_FILTER_JGT			0x20
#define SYSCALL_FILTER_JGE			0x30
#define SYSCALL_FILTER_JSET			0x40

/* Operand sources. */
#define SYSCALL_FILTER_SRC(code)	((code) & 0x08)
#define SYSCALL_FILTER_K			0x00
#define SYSCALL_FILTER_X			0x08

/* Return value sources. */
#define SYSCALL_FILTER_RVAL(code)	((code) & 0x18)
#define SYSCALL_FILTER_A			0x10

/* Register transfers. */
#define SYSCALL_FILTER_MISCOP(code)	((code) & 0xf8)
#define SYSCALL_FILTER_TAX			0x00
#define SYSCALL_FILTER_TXA			0x80


#ifdef __cplusplus
extern "C" {
#endif

/*!	Rejects any program that could loop or read outside the record. */
status_t	syscall_filter_validate(const syscall_filter_insn* instructions,
				uint32 length);

/*!	Evaluates a program that syscall_filter_validate() has accepted. */
uint32		syscall_filter_run(const syscall_filter_insn* instructions,
				uint32 length, const syscall_filter_data* data);

/*!	Returns the more restrictive of two actions. */
uint32		syscall_filter_combine(uint32 first, uint32 second);

#ifdef __cplusplus
}
#endif


#endif	/* _SYSTEM_SYSCALL_FILTER_DEFS_H */
