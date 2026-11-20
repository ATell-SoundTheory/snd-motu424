/* Userspace shim for <linux/kernel.h> */
#ifndef SHIM_LINUX_KERNEL_H
#define SHIM_LINUX_KERNEL_H
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define WARN_ON(c) ({ int __c = !!(c); \
	if (__c) fprintf(stderr, "WARN_ON(%s) at %s:%d\n", #c, __FILE__, __LINE__); \
	__c; })
#endif
