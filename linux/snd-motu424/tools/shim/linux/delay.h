/* Userspace shim for <linux/delay.h> */
#include "../kshim.h"
#ifndef SHIM_LINUX_DELAY_H
#define SHIM_LINUX_DELAY_H
#define udelay(us)	((void)(us))
#endif
