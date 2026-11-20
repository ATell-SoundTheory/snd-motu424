/* Userspace shim for <linux/errno.h> */
#ifndef SHIM_LINUX_ERRNO_H
#define SHIM_LINUX_ERRNO_H
#ifdef __linux__
/* glibc's <errno.h> includes <linux/errno.h>. */
#include_next <linux/errno.h>
#else
#include <errno.h>
#endif
#endif
