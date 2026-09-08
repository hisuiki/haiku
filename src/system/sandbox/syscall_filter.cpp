/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <syscall_filter_defs.h>

#include <string.h>


/*!	Classic BPF interpreter and verifier for syscall filtering.

	Two properties have to hold for every program the kernel is willing to
	run, and both are established here, before the program is ever evaluated,
	so that the interpreter itself needs no bounds checks in its inner loop:

	  - Termination. Jumps may only move forward, so a program cannot loop and
	    is bounded by its own instruction count.
	  - Memory safety. Loads may only address the syscall_filter_data record
	    or the scratch memory, at in-range, correctly aligned offsets.

	A rejected program is never installed, so a caller cannot use a malformed
	filter to weaken a filter it already carries.
*/


static const uint32 kDataSize = (uint32)sizeof(syscall_filter_data);


/*!	Checks a single instruction in isolation, plus its jump targets.

	\param index The position of \a insn, needed to resolve relative jumps.
	\param length The program length, the exclusive upper bound for a target.
	\return \c true if the instruction may be run.
*/
static bool
check_instruction(const syscall_filter_insn& insn, uint32 index, uint32 length)
{
	const uint16 code = insn.code;

	switch (SYSCALL_FILTER_CLASS(code)) {
		case SYSCALL_FILTER_LD:
		case SYSCALL_FILTER_LDX:
			switch (SYSCALL_FILTER_MODE(code)) {
				case SYSCALL_FILTER_IMM:
					return true;

				case SYSCALL_FILTER_ABS:
					// Only whole, aligned words inside the record. Narrower
					// loads would let a program observe the endianness of a
					// field, and so behave differently per architecture for
					// the same policy.
					if (SYSCALL_FILTER_SIZE(code) != SYSCALL_FILTER_W)
						return false;
					if ((insn.k % 4) != 0)
						return false;
					// Guard the addition itself: k is attacker-controlled and
					// k + 4 would wrap for k near UINT32_MAX, letting an
					// out-of-range offset pass a naive bounds check.
					return insn.k <= kDataSize - 4;

				case SYSCALL_FILTER_MEM:
					return insn.k < SYSCALL_FILTER_MEMWORDS;

				// Indirect, header-length and byte-multiplier loads all read
				// a packet that does not exist in this context.
				default:
					return false;
			}

		case SYSCALL_FILTER_ST:
		case SYSCALL_FILTER_STX:
			return insn.k < SYSCALL_FILTER_MEMWORDS;

		case SYSCALL_FILTER_ALU:
			switch (SYSCALL_FILTER_OP(code)) {
				case SYSCALL_FILTER_NEG:
					return true;

				case SYSCALL_FILTER_DIV:
				case SYSCALL_FILTER_MOD:
					// A constant zero divisor can never be anything but a
					// fault, so reject it outright; a divisor taken from X is
					// only known at run time and is checked there.
					if (SYSCALL_FILTER_SRC(code) == SYSCALL_FILTER_K
						&& insn.k == 0) {
						return false;
					}
					return true;

				case SYSCALL_FILTER_ADD:
				case SYSCALL_FILTER_SUB:
				case SYSCALL_FILTER_MUL:
				case SYSCALL_FILTER_OR:
				case SYSCALL_FILTER_AND:
				case SYSCALL_FILTER_LSH:
				case SYSCALL_FILTER_RSH:
				case SYSCALL_FILTER_XOR:
					return true;

				default:
					return false;
			}

		case SYSCALL_FILTER_JMP:
		{
			// Targets are relative to the following instruction. Computing
			// them in 64 bits keeps the sum itself from overflowing.
			const uint64 next = (uint64)index + 1;

			if (SYSCALL_FILTER_OP(code) == SYSCALL_FILTER_JA)
				return next + insn.k < length;

			switch (SYSCALL_FILTER_OP(code)) {
				case SYSCALL_FILTER_JEQ:
				case SYSCALL_FILTER_JGT:
				case SYSCALL_FILTER_JGE:
				case SYSCALL_FILTER_JSET:
					return next + insn.jt < length && next + insn.jf < length;

				default:
					return false;
			}
		}

		case SYSCALL_FILTER_RET:
			return SYSCALL_FILTER_RVAL(code) == SYSCALL_FILTER_K
				|| SYSCALL_FILTER_RVAL(code) == SYSCALL_FILTER_A;

		case SYSCALL_FILTER_MISC:
			return SYSCALL_FILTER_MISCOP(code) == SYSCALL_FILTER_TAX
				|| SYSCALL_FILTER_MISCOP(code) == SYSCALL_FILTER_TXA;

		default:
			return false;
	}
}


/*!	Verifies a whole program.

	\return \c B_OK if every instruction is safe to run, \c B_BAD_VALUE if not.
*/
status_t
syscall_filter_validate(const syscall_filter_insn* instructions, uint32 length)
{
	if (instructions == NULL || length == 0
		|| length > SYSCALL_FILTER_MAX_INSNS) {
		return B_BAD_VALUE;
	}

	// Falling off the end of a program has no defined action, so require the
	// last instruction to be one that cannot fall through.
	if (SYSCALL_FILTER_CLASS(instructions[length - 1].code)
			!= SYSCALL_FILTER_RET) {
		return B_BAD_VALUE;
	}

	for (uint32 i = 0; i < length; i++) {
		if (!check_instruction(instructions[i], i, length))
			return B_BAD_VALUE;
	}

	return B_OK;
}


/*!	Runs a verified program against \a data.

	Must only be called for a program that syscall_filter_validate() has
	accepted; it relies on that verification rather than re-checking bounds
	per instruction.

	\return The action to apply, one of SYSCALL_FILTER_RET_*.
*/
uint32
syscall_filter_run(const syscall_filter_insn* instructions, uint32 length,
	const syscall_filter_data* data)
{
	uint32 a = 0;
	uint32 x = 0;
	uint32 memory[SYSCALL_FILTER_MEMWORDS];

	// Scratch memory is readable before it is written, so it has to start out
	// as something that cannot carry information between two filter runs.
	memset(memory, 0, sizeof(memory));

	for (uint32 pc = 0; pc < length; pc++) {
		const syscall_filter_insn& insn = instructions[pc];
		const uint16 code = insn.code;

		switch (SYSCALL_FILTER_CLASS(code)) {
			case SYSCALL_FILTER_LD:
				switch (SYSCALL_FILTER_MODE(code)) {
					case SYSCALL_FILTER_IMM:
						a = insn.k;
						break;
					case SYSCALL_FILTER_ABS:
						memcpy(&a, (const uint8*)data + insn.k, sizeof(a));
						break;
					case SYSCALL_FILTER_MEM:
						a = memory[insn.k];
						break;
				}
				break;

			case SYSCALL_FILTER_LDX:
				switch (SYSCALL_FILTER_MODE(code)) {
					case SYSCALL_FILTER_IMM:
						x = insn.k;
						break;
					case SYSCALL_FILTER_ABS:
						memcpy(&x, (const uint8*)data + insn.k, sizeof(x));
						break;
					case SYSCALL_FILTER_MEM:
						x = memory[insn.k];
						break;
				}
				break;

			case SYSCALL_FILTER_ST:
				memory[insn.k] = a;
				break;

			case SYSCALL_FILTER_STX:
				memory[insn.k] = x;
				break;

			case SYSCALL_FILTER_ALU:
			{
				if (SYSCALL_FILTER_OP(code) == SYSCALL_FILTER_NEG) {
					// Negate through an unsigned type: -a on a signed int is
					// undefined for the most negative value.
					a = (uint32)(0u - a);
					break;
				}

				const uint32 operand
					= SYSCALL_FILTER_SRC(code) == SYSCALL_FILTER_X
						? x : insn.k;

				switch (SYSCALL_FILTER_OP(code)) {
					case SYSCALL_FILTER_ADD:
						a += operand;
						break;
					case SYSCALL_FILTER_SUB:
						a -= operand;
						break;
					case SYSCALL_FILTER_MUL:
						a *= operand;
						break;
					case SYSCALL_FILTER_DIV:
						// Only reachable with a zero operand when it came
						// from X, which the verifier cannot rule out.
						if (operand == 0)
							return SYSCALL_FILTER_RET_KILL_PROCESS;
						a /= operand;
						break;
					case SYSCALL_FILTER_MOD:
						if (operand == 0)
							return SYSCALL_FILTER_RET_KILL_PROCESS;
						a %= operand;
						break;
					case SYSCALL_FILTER_OR:
						a |= operand;
						break;
					case SYSCALL_FILTER_AND:
						a &= operand;
						break;
					case SYSCALL_FILTER_LSH:
						// A shift of 32 or more is undefined in C, and in
						// practice masks on x86, which would make the same
						// program behave differently across architectures.
						a = operand >= 32 ? 0 : a << operand;
						break;
					case SYSCALL_FILTER_RSH:
						a = operand >= 32 ? 0 : a >> operand;
						break;
					case SYSCALL_FILTER_XOR:
						a ^= operand;
						break;
				}
				break;
			}

			case SYSCALL_FILTER_JMP:
			{
				if (SYSCALL_FILTER_OP(code) == SYSCALL_FILTER_JA) {
					pc += insn.k;
					break;
				}

				const uint32 operand
					= SYSCALL_FILTER_SRC(code) == SYSCALL_FILTER_X
						? x : insn.k;
				bool taken = false;

				switch (SYSCALL_FILTER_OP(code)) {
					case SYSCALL_FILTER_JEQ:
						taken = a == operand;
						break;
					case SYSCALL_FILTER_JGT:
						taken = a > operand;
						break;
					case SYSCALL_FILTER_JGE:
						taken = a >= operand;
						break;
					case SYSCALL_FILTER_JSET:
						taken = (a & operand) != 0;
						break;
				}

				pc += taken ? insn.jt : insn.jf;
				break;
			}

			case SYSCALL_FILTER_RET:
				return SYSCALL_FILTER_RVAL(code) == SYSCALL_FILTER_A
					? a : insn.k;

			case SYSCALL_FILTER_MISC:
				if (SYSCALL_FILTER_MISCOP(code) == SYSCALL_FILTER_TAX)
					x = a;
				else
					a = x;
				break;
		}
	}

	// Unreachable for a verified program, whose last instruction returns.
	return SYSCALL_FILTER_RET_KILL_PROCESS;
}


/*!	Combines the results of two filters.

	Filters are additive, so installing one may never widen what a team can
	do. The action values are ordered so that the more restrictive one is
	numerically smaller, and taking the smaller of the two is what makes that
	guarantee hold.

	The comparison has to be signed. Kill-the-process is 0x80000000, chosen to
	match Linux, and as an unsigned value that is the *largest* action there
	is -- comparing unsigned would make the most severe action lose to every
	other one, including allow. Read as a signed integer it is the smallest,
	which is the ordering the values were picked for.

	Only the action half is compared, but the whole value is returned, so that
	the data an errno or trap action carries survives the choice.
*/
uint32
syscall_filter_combine(uint32 first, uint32 second)
{
	const int32 firstAction = (int32)(first & SYSCALL_FILTER_RET_ACTION_MASK);
	const int32 secondAction = (int32)(second & SYSCALL_FILTER_RET_ACTION_MASK);

	return firstAction < secondAction ? first : second;
}
