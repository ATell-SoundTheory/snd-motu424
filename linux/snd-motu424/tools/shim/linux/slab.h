/* Userspace shim for <linux/slab.h> */
#include <stdlib.h>
#define GFP_KERNEL 0
#define kmalloc(sz, gfp) malloc(sz)
#define kfree(p) free(p)
#define kvmalloc(sz, gfp) malloc(sz)
#define kvfree(p) free(p)
