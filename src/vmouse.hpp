#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Mouse output for the aim assist and triggerbot. Injects through
// the kernel module when loaded, otherwise a uinput device.
//
// linux/uinput.h is kept in vmouse.cpp on purpose: it pulls in
// linux/input-event-codes.h, whose KEY_* macros collide with raylib's
// KeyboardKey enumerators. Nothing that includes raylib may include it.

// Mouse output - two backends, kernel preferred
class VMouse {
public:
    bool open();
    void close();
    ~VMouse();

    bool ready() const { return kmodFd_ >= 0 || fd_ >= 0; }

    // True when motion is going through the kernel module (real pointer, no
    // node) rather than the enumerable uinput device.
    bool usingKernel() const { return kmodFd_ >= 0; }

    // Relative move. Sub-pixel remainders are carried between calls: smoothing
    // often asks for well under one count per frame, and truncating that to 0
    // would stall the aim completely near the target.
    void moveRel(double dx, double dy);

    // Left mouse button, for the triggerbot. Kernel path clicks through the real
    // pointer; uinput path clicks the virtual device (which declared BTN_LEFT).
    void press(bool down);

private:
    bool fail(const char* what);
    void emit(unsigned short type, unsigned short code, int value);

    int    fd_     = -1;   // uinput fd (fallback path)
    int    kmodFd_ = -1;   // /dev/suite_kmod fd (kernel-injection path)
    double accX_ = 0.0, accY_ = 0.0;
};

inline VMouse g_vmouse;
