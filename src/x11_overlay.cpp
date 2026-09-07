// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Xlib implementation of the overlay window and of global input polling.
#include "x11_overlay.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/shape.h>

#include <cstdio>
#include <unistd.h>
#include <vector>

namespace ovl {
namespace {

Display* g_dpy = nullptr;
::Window g_win = 0;          // our overlay window, once adopted

int ignoreError(Display* d, XErrorEvent* e) {
    char buf[128] = {0};
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    printf("[x11] (ignored) %s  request %d.%d\n",
           buf, e->request_code, e->minor_code);
    return 0;
}

// Breadth-first walk of the window tree, collecting mapped windows whose
// _NET_WM_PID matches. The game nests its drawable inside the top-level client,
// and the window manager wraps that again in a decoration frame. Aligning to
// the wrong one of those puts the whole ESP out by the titlebar height, so a
// window carrying WM_STATE -- the ICCCM mark of a real top-level client -- is
// always preferred over a plain child, and only then does area break the tie.
::Window findByPid(pid_t pid, int* outW = nullptr, int* outH = nullptr) {
    Atom pidAtom = XInternAtom(g_dpy, "_NET_WM_PID", True);
    if (!pidAtom) return 0;
    Atom wmStateAtom = XInternAtom(g_dpy, "WM_STATE", True);
    ::Window best = 0;
    unsigned bestArea = 0;
    int bestRank = -1;

    std::vector< ::Window> queue{ DefaultRootWindow(g_dpy) };
    for (size_t qi = 0; qi < queue.size() && qi < 4096; qi++) {
        ::Window w = queue[qi];
        ::Window root = 0, parent = 0, *kids = nullptr;
        unsigned nkids = 0;
        if (XQueryTree(g_dpy, w, &root, &parent, &kids, &nkids)) {
            for (unsigned i = 0; i < nkids; i++) queue.push_back(kids[i]);
            if (kids) XFree(kids);
        }
        Atom type = 0; int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(g_dpy, w, pidAtom, 0, 1, False, XA_CARDINAL,
                               &type, &fmt, &nitems, &after, &data) != Success)
            continue;
        if (!data) continue;
        pid_t wpid = (pid_t)(*(unsigned long*)data);
        XFree(data);
        if (pid > 0 && wpid != pid) continue;

        XWindowAttributes at{};
        if (!XGetWindowAttributes(g_dpy, w, &at)) continue;
        if (at.map_state != IsViewable) continue;
        if (at.c_class != InputOutput) continue;

        int rank = 0;
        if (wmStateAtom) {
            Atom st = 0; int sf = 0;
            unsigned long sn = 0, sa = 0;
            unsigned char* sd = nullptr;
            if (XGetWindowProperty(g_dpy, w, wmStateAtom, 0, 1, False,
                                   AnyPropertyType, &st, &sf, &sn, &sa, &sd)
                    == Success && sd) {
                rank = 1;                       // a managed top-level client
                XFree(sd);
            }
        }
        unsigned area = (unsigned)at.width * (unsigned)at.height;
        if (rank > bestRank || (rank == bestRank && area > bestArea)) {
            bestRank = rank; bestArea = area; best = w;
            if (outW) *outW = at.width;
            if (outH) *outH = at.height;
        }
    }
    return best;
}

KeySym keysymFor(int key) {
    switch (key) {
        case KeyInsert: return XK_Insert;
        case KeyEnd:    return XK_End;
        default:        return XK_Home;
    }
}

}  // namespace

bool init() {
    XInitThreads();
    XSetErrorHandler(ignoreError);
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        printf("[x11] XOpenDisplay failed: no click-through, no positioning.\n"
               "[x11] Run with: DISPLAY=:0 ./TheFinals --pid <PID>\n");
        return false;
    }
    return true;
}

void shutdown() {
    if (!g_dpy) return;
    // Never exit holding the pointer: that would leave the desktop unusable.
    XUngrabPointer(g_dpy, CurrentTime);
    XUngrabKeyboard(g_dpy, CurrentTime);
    XFlush(g_dpy);
    XCloseDisplay(g_dpy);
    g_dpy = nullptr;
    g_win = 0;
}

bool gameRect(pid_t gamePid, int& x, int& y, int& w, int& h) {
    if (!g_dpy) return false;
    ::Window gw = findByPid(gamePid);
    static ::Window announced = 0;
    static bool warned = false;
    if (!gw) {
        if (!warned) {
            printf("[x11] no mapped window has _NET_WM_PID %d yet\n", (int)gamePid);
            warned = true;
        }
        return false;
    }
    if (gw != announced) {
        printf("[x11] game window is 0x%lx\n", gw);
        announced = gw;
        warned = false;
    }
    XWindowAttributes at{};
    if (!XGetWindowAttributes(g_dpy, gw, &at)) return false;
    // XWindowAttributes x/y are parent-relative; the overlay needs screen space.
    ::Window child = 0;
    if (!XTranslateCoordinates(g_dpy, gw, DefaultRootWindow(g_dpy),
                               0, 0, &x, &y, &child))
        return false;
    w = at.width;
    h = at.height;
    return w > 16 && h > 16;
}

bool adoptOwnWindow(const char* title) {
    if (!g_dpy) return false;
    g_win = findByPid(getpid());
    if (!g_win) {
        printf("[x11] could not find our own window in the tree; "
               "overlay properties skipped\n");
        return false;
    }
    if (title) XStoreName(g_dpy, g_win, title);

    XWindowAttributes at{};
    if (XGetWindowAttributes(g_dpy, g_win, &at))
        printf("[x11] overlay window 0x%lx, depth %d%s\n", g_win, at.depth,
               at.depth == 32 ? " (ARGB)"
                              : " -- NOT 32-bit: the overlay will be OPAQUE");

    // override-redirect must be set while the window is UNMAPPED: a window
    // manager reads the flag when the window is mapped and ignores it after.
    // While the window is managed it carries a decoration frame, moves are
    // relative to that frame rather than to the screen, and the WM is free to
    // place it where it likes. Unmapped it is neither framed nor reparented,
    // and absolute coordinates land exactly where they are asked to.
    Atom wmWindowType    = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom wmWindowTypeTip = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    XChangeProperty(g_dpy, g_win, wmWindowType, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&wmWindowTypeTip, 1);

    XUnmapWindow(g_dpy, g_win);
    XSync(g_dpy, False);
    XSetWindowAttributes ora{};
    ora.override_redirect = 1;
    XChangeWindowAttributes(g_dpy, g_win, CWOverrideRedirect, &ora);
    XMapRaised(g_dpy, g_win);
    XSync(g_dpy, False);

    // Confirm it actually came back unmanaged; if a WM still owns it the
    // positioning below will be off and it is worth saying so out loud.
    XWindowAttributes after{};
    ::Window root = 0, parent = 0, *kids = nullptr; unsigned nk = 0;
    bool reparented = false;
    if (XQueryTree(g_dpy, g_win, &root, &parent, &kids, &nk)) {
        reparented = (parent != DefaultRootWindow(g_dpy));
        if (kids) XFree(kids);
    }
    XGetWindowAttributes(g_dpy, g_win, &after);
    printf("[x11] overlay unmanaged: override_redirect=%d parent=%s\n",
           after.override_redirect, reparented ? "WM FRAME (unexpected)" : "root");

    // Click-through: an empty input shape, so clicks reach the game underneath.
    setClickThrough(true);

    XFlush(g_dpy);
    printf("[x11] overlay ready: click-through, on top, no taskbar entry\n");
    return true;
}

void moveResize(int x, int y, int w, int h) {
    if (!g_dpy || !g_win) return;
    XMoveResizeWindow(g_dpy, g_win, x, y, (unsigned)w, (unsigned)h);
    XSync(g_dpy, False);

    // Report where it actually landed. On an unmanaged window this always
    // matches; a mismatch means something is still framing or moving us, and
    // that shows up as the whole ESP being offset by exactly that much.
    int ax = 0, ay = 0; ::Window child = 0;
    if (XTranslateCoordinates(g_dpy, g_win, DefaultRootWindow(g_dpy),
                              0, 0, &ax, &ay, &child)
        && (ax != x || ay != y)) {
        // Rate-limited: this runs twice a second, and a persistent offset
        // would otherwise bury every other line in the log.
        static int lastDx = 0, lastDy = 0;
        if ((x - ax) != lastDx || (y - ay) != lastDy) {
            printf("[x11] overlay landed at %d,%d but %d,%d was asked for "
                   "- correcting by %d,%d\n", ax, ay, x, y, x - ax, y - ay);
            lastDx = x - ax; lastDy = y - ay;
        }
        XMoveWindow(g_dpy, g_win, x + (x - ax), y + (y - ay));
        XSync(g_dpy, False);
    }
}

void raise() {
    if (!g_dpy || !g_win) return;
    XRaiseWindow(g_dpy, g_win);
    XFlush(g_dpy);
}

void setClickThrough(bool through) {
    if (!g_dpy || !g_win) return;
    if (through) {
        Region empty = XCreateRegion();
        XShapeCombineRegion(g_dpy, g_win, ShapeInput, 0, 0, empty, ShapeSet);
        XDestroyRegion(empty);
    } else {
        // 0L = None: reset the input shape to the whole window.
        XShapeCombineMask(g_dpy, g_win, ShapeInput, 0, 0, 0L, ShapeSet);
    }
    XFlush(g_dpy);
}

void grabInput(bool grab) {
    if (!g_dpy || !g_win) return;
    if (grab) {
        // owner_events = True, so the events still reach the overlay's own
        // event selection and raylib sees them; the grab only redirects them
        // away from the game while the menu is up.
        int r = XGrabPointer(g_dpy, g_win, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, 0L, 0L, CurrentTime);
        const char* why = r == 0 ? "ok"
                        : r == 1 ? "AlreadyGrabbed (game holds it)"
                        : r == 2 ? "InvalidTime" : r == 3 ? "NotViewable" : "Frozen";
        printf("[x11] grab pointer: %s\n", why);
        XGrabKeyboard(g_dpy, g_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    } else {
        XUngrabPointer(g_dpy, CurrentTime);
        XUngrabKeyboard(g_dpy, CurrentTime);
        printf("[x11] pointer/keyboard released back to the game\n");
    }
    XFlush(g_dpy);
}

bool lmbDown() {
    if (!g_dpy) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_dpy, DefaultRootWindow(g_dpy),
                       &r, &c, &rx, &ry, &wx, &wy, &mask)) return false;
    return (mask & Button1Mask) != 0;
}

bool rmbDown() {
    if (!g_dpy) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_dpy, DefaultRootWindow(g_dpy),
                       &r, &c, &rx, &ry, &wx, &wy, &mask)) return false;
    return (mask & Button3Mask) != 0;      // Button2 is middle, not right
}

bool keyDown(int key) {
    if (!g_dpy) return false;
    char keys[32];
    XQueryKeymap(g_dpy, keys);
    KeyCode kc = XKeysymToKeycode(g_dpy, keysymFor(key));
    if (!kc) return false;
    return (keys[kc / 8] & (1 << (kc % 8))) != 0;
}

}  // namespace ovl
