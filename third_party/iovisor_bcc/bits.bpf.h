/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/*
 * From iovisor's bcc/libbpf-tools/bits.bpf.h.
 *
 * These are very small, and it's not worth getting a dependency on
 * third_party/bcc/.  The tools including this header are similar in style to
 * libbpf-tools, which are intended to be built from within the
 * bcc/libbpf-tools/ directory.
 */

#ifndef GHOST_LIB_BPF_BITS_BPF_H_
#define GHOST_LIB_BPF_BITS_BPF_H_

static __always_inline u64 log2(u32 v)
{
	u32 shift, r;

	r = (v > 0xFFFF) << 4; v >>= r;
	shift = (v > 0xFF) << 3; v >>= shift; r |= shift;
	shift = (v > 0xF) << 2; v >>= shift; r |= shift;
	shift = (v > 0x3) << 1; v >>= shift; r |= shift;
	r |= (v >> 1);

	return r;
}

static __always_inline u64 log2l(u64 v)
{
	u32 hi = v >> 32;

	if (hi)
		return log2(hi) + 32;
	else
		return log2(v);
}

#endif  // GHOST_LIB_BPF_BITS_BPF_H_
