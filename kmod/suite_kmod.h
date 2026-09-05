// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
/* suite_kmod.h - shared ioctl contract for the kernel-module memory backend.
 *
 * ONE header, compiled by BOTH the kernel module and the external. That is the
 * point: the struct layout and the ioctl number are computed from the same
 * source on both sides, so they cannot drift. Do not fork this into two copies.
 *
 * The struct is the wire contract exactly as specified: int + three u64. On
 * every Linux/LP64 target `int` is 32-bit and the u64s are 8-byte aligned, so
 * the layout is 4 + pad(4) + 8 + 8 + 8 = 32 bytes in kernel and userspace
 * alike, and _IOWR bakes that size into the command number.
 */
#ifndef SUITE_KMOD_H
#define SUITE_KMOD_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/ioctl.h>
#  define SUITE_U64 __u64
#else
#  include <stdint.h>
#  include <sys/ioctl.h>
#  define SUITE_U64 uint64_t
#endif

#define SUITE_DEVICE_PATH "/dev/suite_kmod"
#define SUITE_MAGIC       'S'

struct suite_read_req {
    int       target_pid;   /* PID to read FROM                              */
    SUITE_U64 address;       /* address in the target                         */
    SUITE_U64 size;          /* bytes to read                                 */
    SUITE_U64 out_ptr;       /* buffer in OUR process to receive the bytes    */
};

/* SUITE_READ_MEM: read `size` bytes from `target_pid` at `address` into the
 * caller's `out_ptr`. Returns the number of bytes actually read (>= 0), or a
 * negative errno. A short return means a page in the range was unreadable -
 * the same semantics access_process_vm() already gives. */
#define SUITE_READ_MEM    _IOWR(SUITE_MAGIC, 1, struct suite_read_req)

/* Upper bound on a single request, so a bad `size` cannot ask the kernel to
 * allocate unboundedly. The external's largest reads are the ~32 MB region
 * sweeps; 64 MB leaves headroom. Bigger reads must be chunked by the caller. */
#define SUITE_MAX_READ    (64u * 1024u * 1024u)

/* ── mouse injection ──────────────────────────────────────────────────────
 * The module binds to the real relative pointer(s) as an input handler and
 * reports motion THROUGH the device the user is actively using. So the events
 * are born in kernel space, carry the real mouse's identity, and create NO new
 * device node - unlike the uinput puppet, which is enumerable in
 * /proc/bus/input/devices. If no pointer is bound the ioctl returns -ENODEV and
 * the external falls back to uinput. */
struct suite_move_req {
    int dx;
    int dy;
};

/* SUITE_MOVE_MOUSE: report a relative (dx, dy) through the active pointer.
 * Returns 0, or -ENODEV if no pointer is bound. */
#define SUITE_MOVE_MOUSE    _IOW(SUITE_MAGIC, 2, struct suite_move_req)

/* SUITE_MOUSE_STATUS: writes 1 if a pointer is bound and injection is live,
 * else 0. Used by the external to decide whether to use this path or uinput. */
#define SUITE_MOUSE_STATUS  _IOR(SUITE_MAGIC, 3, int)

/* SUITE_CLICK: press (down=1) or release (down=0) the left mouse button through
 * the active pointer. Used by the triggerbot. Returns 0, or -ENODEV. */
struct suite_click_req { int down; };
#define SUITE_CLICK         _IOW(SUITE_MAGIC, 4, struct suite_click_req)

#endif /* SUITE_KMOD_H */
