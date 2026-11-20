/* Userspace stand-ins for the kernel API used by motu424.h and motu424_iface.c. */
#ifndef SHIM_KSHIM_H
#define SHIM_KSHIM_H
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>

typedef u32 __le32;
typedef u64 dma_addr_t;
#define __iomem
#define __maybe_unused	__attribute__((unused))
#define BIT(n)		(1u << (n))
#define BIT_ULL(n)	(1ull << (n))
/* the tests run on little-endian hosts */
#define cpu_to_le32(x)	((__le32)(x))
#define le32_to_cpu(x)	((u32)(x))
#define swab16(x)	((u16)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))

struct device { int unused; };
struct pci_dev { struct device dev; int irq; };
typedef struct { int unused; } spinlock_t;
struct mutex { int unused; };
#define mutex_lock(m)			((void)(m))
#define mutex_unlock(m)			((void)(m))
#define spin_lock_irqsave(l, f)		((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f)	((void)(l), (void)(f))
#define msleep(ms)			((void)(ms))
#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define kmalloc_array(n, size, gfp)	calloc(n, size)
#define hweight64(v)			__builtin_popcountll(v)
#define READ_ONCE(x)	(*(const volatile typeof(x) *)&(x))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define set_bit(n, p)	((*(p) |= 1ul << (n)), 0)
#define clear_bit(n, p)	((*(p) &= ~(1ul << (n))), 0)
#define test_bit(n, p)	(((*(p) >> (n)) & 1ul) != 0)

static inline ssize_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);

	if (size) {
		size_t copy = n < size - 1 ? n : size - 1;

		memcpy(dst, src, copy);
		dst[copy] = 0;
	}
	return n < size ? (ssize_t)n : -E2BIG;
}

static inline void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	(void)dev;
	(void)flags;
	return calloc(1, size);
}

struct snd_card { char longname[80]; };
struct snd_pcm;
struct snd_pcm_substream;
struct snd_info_buffer;
struct snd_info_entry { void *private_data; };
#define snd_card_ro_proc_new(card, name, data, fn) \
	((void)(card), (void)(data), (void)(fn))

/* Messages are counted, not formatted (the kernel's %*ph has no libc match). */
extern unsigned int kshim_warnings;
extern int kshim_verbose;

static inline void kshim_log(int warn, const struct device *dev,
			     const char *fmt, ...)
{
	(void)dev;
	kshim_warnings += warn;
	if (kshim_verbose)
		fprintf(stderr, "%s: %s", warn ? "warn" : "info", fmt);
}
#define dev_info(dev, ...)	kshim_log(0, dev, __VA_ARGS__)
#define dev_warn(dev, ...)	kshim_log(1, dev, __VA_ARGS__)
#define dev_err_ratelimited(dev, ...)	kshim_log(1, dev, __VA_ARGS__)
#define dev_err(dev, ...)	kshim_log(1, dev, __VA_ARGS__)

static inline void snd_iprintf(struct snd_info_buffer *buf, const char *fmt, ...)
{
	(void)buf;
	(void)fmt;
}

static inline int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0 || !size)
		return 0;
	return (size_t)n >= size ? (int)size - 1 : n;
}
#endif
