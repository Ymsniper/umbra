#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Mouse output for the aim assist and triggerbot. Injects through
// the kernel module when loaded, otherwise a uinput device.
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include "../kmod/suite_kmod.h"   // optional kernel mouse-injection backend

// Mouse output - two backends, kernel preferred
class VMouse {
public:
    bool open() {
        if (kmodFd_ >= 0 || fd_ >= 0) return true;

        // Prefer the kernel module: it reports motion THROUGH the real pointer
        // from kernel space, so there is no new /dev/input node and the events
        // carry the real mouse's identity. Only used if a pointer is actually
        // bound (SUITE_MOUSE_STATUS == 1); otherwise fall back to uinput.
        int kf = ::open(SUITE_DEVICE_PATH, O_RDWR);
        if (kf >= 0) {
            int ready = 0;
            if (::ioctl(kf, SUITE_MOUSE_STATUS, &ready) == 0 && ready == 1) {
                kmodFd_ = kf;
                printf("[vmouse] kernel injection into the real pointer "
                       "(no device node, no uinput)\n");
                return true;
            }
            ::close(kf);   // module present but no pointer bound - use uinput
            printf("[vmouse] kernel module has no bound pointer -- using uinput\n");
        }

        fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) fd_ = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) {
            printf("[vmouse] cannot open /dev/uinput (%s)\n", strerror(errno));
            printf("[vmouse]   try: sudo modprobe uinput\n");
            printf("[vmouse]   and be in the 'uinput' or 'input' group\n");
            return false;
        }

        if (ioctl(fd_, UI_SET_EVBIT,  EV_REL) < 0) return fail("EV_REL");
        if (ioctl(fd_, UI_SET_EVBIT,  EV_SYN) < 0) return fail("EV_SYN");
        if (ioctl(fd_, UI_SET_RELBIT, REL_X)  < 0) return fail("REL_X");
        if (ioctl(fd_, UI_SET_RELBIT, REL_Y)  < 0) return fail("REL_Y");

        // libinput needs a button capability to classify this as a mouse, or
        // REL_X/REL_Y are ignored entirely. Declared but never emitted - this
        // device only ever moves the cursor, it does not click.
        if (ioctl(fd_, UI_SET_EVBIT,  EV_KEY)   < 0) return fail("EV_KEY");
        if (ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT) < 0) return fail("BTN_LEFT");

        struct uinput_user_dev dev;
        memset(&dev, 0, sizeof dev);
        snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "virtual pointer");
        dev.id.bustype = BUS_USB;
        dev.id.vendor  = 0x1234;
        dev.id.product = 0x5679;
        dev.id.version = 1;
        if (::write(fd_, &dev, sizeof dev) < 0) return fail("write(uinput_user_dev)");
        if (ioctl(fd_, UI_DEV_CREATE, 0) < 0)   return fail("UI_DEV_CREATE");

        usleep(100000);          // let udev settle before the first event
        printf("[vmouse] virtual pointer created\n");
        return true;
    }

    void close() {
        if (kmodFd_ >= 0) { ::close(kmodFd_); kmodFd_ = -1; }
        if (fd_ < 0) return;
        ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
        fd_ = -1;
    }

    bool ready() const { return kmodFd_ >= 0 || fd_ >= 0; }

    // True when motion is going through the kernel module (real pointer, no
    // node) rather than the enumerable uinput device.
    bool usingKernel() const { return kmodFd_ >= 0; }

    // Relative move. Sub-pixel remainders are carried between calls: smoothing
    // often asks for well under one count per frame, and truncating that to 0
    // would stall the aim completely near the target.
    void moveRel(double dx, double dy) {
        if (kmodFd_ < 0 && fd_ < 0) return;
        accX_ += dx; accY_ += dy;
        int ix = (int)accX_, iy = (int)accY_;
        accX_ -= ix; accY_ -= iy;
        if (!ix && !iy) return;

        if (kmodFd_ >= 0) {
            // one syscall, injected through the real pointer in kernel space
            struct suite_move_req req;
            req.dx = ix;
            req.dy = iy;
            ::ioctl(kmodFd_, SUITE_MOVE_MOUSE, &req);
            return;
        }
        if (ix) emit(EV_REL, REL_X, ix);
        if (iy) emit(EV_REL, REL_Y, iy);
        emit(EV_SYN, SYN_REPORT, 0);
    }

    // Left mouse button, for the triggerbot. Kernel path clicks through the real
    // pointer; uinput path clicks the virtual device (which declared BTN_LEFT).
    void press(bool down) {
        if (kmodFd_ >= 0) {
            struct suite_click_req req; req.down = down ? 1 : 0;
            ::ioctl(kmodFd_, SUITE_CLICK, &req);
            return;
        }
        if (fd_ < 0) return;
        emit(EV_KEY, BTN_LEFT, down ? 1 : 0);
        emit(EV_SYN, SYN_REPORT, 0);
    }

    ~VMouse() { close(); }

private:
    bool fail(const char* what) {
        printf("[vmouse] ioctl %s failed: %s\n", what, strerror(errno));
        ::close(fd_); fd_ = -1;
        return false;
    }
    void emit(unsigned short type, unsigned short code, int value) {
        struct input_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = type; ev.code = code; ev.value = value;
        if (::write(fd_, &ev, sizeof ev) < 0) { /* non-blocking; drop */ }
    }

    int    fd_     = -1;   // uinput fd (fallback path)
    int    kmodFd_ = -1;   // /dev/suite_kmod fd (kernel-injection path)
    double accX_ = 0.0, accY_ = 0.0;
};

inline VMouse g_vmouse;
