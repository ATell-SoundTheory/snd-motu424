/* Userspace shim for <linux/math64.h> */
#ifndef SHIM_LINUX_MATH64_H
#define SHIM_LINUX_MATH64_H
#include "../kshim.h"

static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return dividend / divisor;
}

static inline u64 div64_u64(u64 dividend, u64 divisor)
{
	return dividend / divisor;
}

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
#endif
