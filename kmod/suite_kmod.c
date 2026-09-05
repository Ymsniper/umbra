// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
/*
 * suite_kmod - a character device that reads another process's memory from
 * kernel space via access_process_vm(). Because the read happens in the kernel,
 * it is NOT subject to Yama's ptrace_scope: it works with
 * /proc/sys/kernel/yama/ptrace_scope set to 1, 2, or 3, where the userspace
 * process_vm_readv() path would be refused.
 *
 * The external opens /dev/suite_kmod at startup and routes its reads through
 * SUITE_READ_MEM. If the device is absent it falls back to process_vm_readv,
 * so this module is purely an optional, drop-in backend.
 *
 * access_process_vm() is EXPORT_SYMBOL_GPL, hence the GPL license below.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/input.h>
#include <linux/spinlock.h>
#include <linux/device.h>

#include "suite_kmod.h"

/* Small reads (8/16/96 bytes are the common case) go on the kernel stack to
 * avoid an allocation on the hot path; larger reads use kvmalloc, which backs
 * multi-MB requests with vmalloc rather than failing on fragmentation. */
#define SUITE_STACK_BUF 1024

static long suite_read_mem(struct suite_read_req *req)
{
	struct task_struct *task;
	struct pid *pid_struct;
	void *kbuf;
	char stackbuf[SUITE_STACK_BUF];
	unsigned long len = (unsigned long)req->size;
	int nread;
	long ret;

	if (len == 0)
		return 0;
	if (len > SUITE_MAX_READ)
		return -EINVAL;

	/* find_get_task_by_vpid() is not exported on every kernel (it is not on
	 * this CachyOS build), so go the long way with symbols that always are:
	 * a get-ref'd struct pid, then the task it names. */
	pid_struct = find_get_pid((pid_t)req->target_pid);
	if (!pid_struct)
		return -ESRCH;
	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task)
		return -ESRCH;

	if (len <= SUITE_STACK_BUF) {
		kbuf = stackbuf;
	} else {
		kbuf = kvmalloc(len, GFP_KERNEL);
		if (!kbuf) {
			put_task_struct(task);
			return -ENOMEM;
		}
	}

	/* gup_flags = 0 -> read (FOLL_WRITE would be a write). Returns the number
	 * of bytes accessed; short at the first unreadable page. */
	nread = access_process_vm(task, (unsigned long)req->address,
				  kbuf, (int)len, 0);
	put_task_struct(task);

	if (nread < 0) {
		ret = nread;
		goto out;
	}
	if (nread > 0 &&
	    copy_to_user((void __user *)(unsigned long)req->out_ptr, kbuf, nread)) {
		ret = -EFAULT;
		goto out;
	}
	ret = nread;   /* bytes actually read; caller compares against size */

out:
	if (kbuf != stackbuf)
		kvfree(kbuf);
	return ret;
}

/* ── mouse injection: report motion through the REAL pointer ───────────────
 *
 * We register as an input handler bound to relative pointers. `suite_target` is
 * the device the user last moved (so injection follows whatever mouse is in
 * use); injected motion goes through it via input_report_rel(), so it carries
 * that device's identity and creates no new node.
 *
 * Locking: suite_lock protects suite_target. It is NEVER held across
 * input_report_rel(), because that dispatches back into suite_event() (under the
 * device's event_lock) which also takes suite_lock - holding it across would
 * invert the lock order and deadlock. Instead we take a device reference under
 * the lock, drop it, inject, then release the reference.
 */
static DEFINE_SPINLOCK(suite_lock);
static struct input_dev *suite_target;

static void suite_event(struct input_handle *handle, unsigned int type,
			unsigned int code, int value)
{
	/* follow the pointer the user is actually moving */
	if (type == EV_REL && (code == REL_X || code == REL_Y) && value != 0) {
		unsigned long flags;

		spin_lock_irqsave(&suite_lock, flags);
		suite_target = handle->dev;
		spin_unlock_irqrestore(&suite_lock, flags);
	}
}

static int suite_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct input_handle *handle;
	unsigned long flags;
	int err;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev     = dev;
	handle->handler = handler;
	handle->name    = "suite_kmod";

	err = input_register_handle(handle);
	if (err)
		goto free;
	err = input_open_device(handle);   /* required to receive suite_event */
	if (err)
		goto unregister;

	spin_lock_irqsave(&suite_lock, flags);
	if (!suite_target)
		suite_target = dev;            /* first pointer, until one moves */
	spin_unlock_irqrestore(&suite_lock, flags);

	pr_info("suite_kmod: bound pointer '%s'\n", dev->name ? dev->name : "?");
	return 0;

unregister:
	input_unregister_handle(handle);
free:
	kfree(handle);
	return err;
}

static void suite_disconnect(struct input_handle *handle)
{
	unsigned long flags;

	spin_lock_irqsave(&suite_lock, flags);
	if (suite_target == handle->dev)
		suite_target = NULL;
	spin_unlock_irqrestore(&suite_lock, flags);

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* Match relative pointers (have EV_REL with REL_X/REL_Y). */
static const struct input_device_id suite_ids[] = {
	{
		.flags  = INPUT_DEVICE_ID_MATCH_EVBIT |
			  INPUT_DEVICE_ID_MATCH_RELBIT,
		.evbit  = { BIT_MASK(EV_REL) },
		.relbit = { BIT_MASK(REL_X) | BIT_MASK(REL_Y) },
	},
	{ },
};
MODULE_DEVICE_TABLE(input, suite_ids);

static struct input_handler suite_input_handler = {
	.event      = suite_event,
	.connect    = suite_connect,
	.disconnect = suite_disconnect,
	.name       = "suite_kmod",
	.id_table   = suite_ids,
};

static long suite_move_mouse(struct suite_move_req *req)
{
	struct input_dev *dev;
	unsigned long flags;

	spin_lock_irqsave(&suite_lock, flags);
	dev = suite_target;
	if (dev)
		get_device(&dev->dev);   /* pin it so it cannot vanish mid-inject */
	spin_unlock_irqrestore(&suite_lock, flags);

	if (!dev)
		return -ENODEV;

	if (req->dx)
		input_report_rel(dev, REL_X, req->dx);
	if (req->dy)
		input_report_rel(dev, REL_Y, req->dy);
	input_sync(dev);

	put_device(&dev->dev);
	return 0;
}

static long suite_click(struct suite_click_req *req)
{
	struct input_dev *dev;
	unsigned long flags;

	spin_lock_irqsave(&suite_lock, flags);
	dev = suite_target;
	if (dev)
		get_device(&dev->dev);
	spin_unlock_irqrestore(&suite_lock, flags);

	if (!dev)
		return -ENODEV;

	input_report_key(dev, BTN_LEFT, req->down ? 1 : 0);
	input_sync(dev);

	put_device(&dev->dev);
	return 0;
}

static long suite_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct suite_read_req req;
	struct suite_move_req mreq;
	struct suite_click_req creq;
	int status;
	unsigned long flags;

	switch (cmd) {
	case SUITE_READ_MEM:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		return suite_read_mem(&req);

	case SUITE_MOVE_MOUSE:
		if (copy_from_user(&mreq, (void __user *)arg, sizeof(mreq)))
			return -EFAULT;
		return suite_move_mouse(&mreq);

	case SUITE_CLICK:
		if (copy_from_user(&creq, (void __user *)arg, sizeof(creq)))
			return -EFAULT;
		return suite_click(&creq);

	case SUITE_MOUSE_STATUS:
		spin_lock_irqsave(&suite_lock, flags);
		status = suite_target ? 1 : 0;
		spin_unlock_irqrestore(&suite_lock, flags);
		if (copy_to_user((void __user *)arg, &status, sizeof(status)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations suite_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = suite_ioctl,
	.compat_ioctl   = suite_ioctl,
};

static struct miscdevice suite_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "suite_kmod",     /* -> /dev/suite_kmod */
	.fops  = &suite_fops,
	/* 0666: the external can run as the normal user because the KERNEL does
	 * the privileged read. NOTE: while loaded, ANY local user can read ANY
	 * process's memory through this node. That is the intended trade for a
	 * research box; tighten via udev or lower this mode if that is not wanted. */
	.mode  = 0666,
};

static int __init suite_init(void)
{
	int rc = misc_register(&suite_dev);

	if (rc) {
		pr_err("suite_kmod: misc_register failed: %d\n", rc);
		return rc;
	}

	/* Mouse injection is optional: if the handler fails to register, the read
	 * backend still works and the external falls back to uinput for motion. */
	rc = input_register_handler(&suite_input_handler);
	if (rc)
		pr_warn("suite_kmod: input handler not registered (%d); "
			"mouse injection unavailable, reads still work\n", rc);

	pr_info("suite_kmod: loaded, %s ready (access_process_vm + mouse inject)\n",
		SUITE_DEVICE_PATH);
	return 0;
}

static void __exit suite_exit(void)
{
	input_unregister_handler(&suite_input_handler);
	misc_deregister(&suite_dev);
	pr_info("suite_kmod: unloaded\n");
}

module_init(suite_init);
module_exit(suite_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("access_process_vm memory-read backend for the external");
MODULE_VERSION("1.0");
