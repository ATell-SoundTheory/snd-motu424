/* Userspace shim for <linux/workqueue.h> */
#ifndef SHIM_LINUX_WORKQUEUE_H
#define SHIM_LINUX_WORKQUEUE_H
#include "../kshim.h"

struct work_struct {
	void (*func)(struct work_struct *work);
};

extern struct work_struct *kshim_pending_work;

#define INIT_WORK(work, callback) ((work)->func = (callback))

static inline bool schedule_work(struct work_struct *work)
{
	kshim_pending_work = work;
	return true;
}

static inline bool cancel_work_sync(struct work_struct *work)
{
	if (kshim_pending_work == work)
		kshim_pending_work = NULL;
	return true;
}

/* delayed work only records whether it is queued; tests run it by hand */
struct delayed_work {
	struct work_struct work;
	bool queued;
};

#define INIT_DELAYED_WORK(dw, callback) \
	((dw)->work.func = (callback), (dw)->queued = false)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define msecs_to_jiffies(ms) ((unsigned long)(ms))

static inline bool schedule_delayed_work(struct delayed_work *dw,
					 unsigned long delay)
{
	(void)delay;
	dw->queued = true;
	return true;
}

static inline bool cancel_delayed_work_sync(struct delayed_work *dw)
{
	bool queued = dw->queued;

	dw->queued = false;
	return queued;
}
#endif
