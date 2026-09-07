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
// Never mapped, never drawn. WM_TAKE_FOCUS needs a real server timestamp, and
// the only way to obtain one is a property change on a window we own -- which
// must not be the overlay, because selecting PropertyChangeMask on that would
// replace the event mask raylib set on it.
::Window g_timeWin = 0;

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
::Window findByPid(pid_t pid, bool requireViewable = true,
                  int* outW = nullptr, int* outH = nullptr) {
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
        if (requireViewable && at.map_state != IsViewable) continue;
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

// A valid server timestamp, from the PropertyNotify our own change generates.
Time serverTime() {
    if (!g_timeWin) return CurrentTime;
    Atom a = XInternAtom(g_dpy, "_UMBRA_TIMESTAMP", False);
    unsigned char zero = 0;
    XChangeProperty(g_dpy, g_timeWin, a, XA_STRING, 8, PropModeReplace, &zero, 1);
    XFlush(g_dpy);
    XEvent ev;
    for (int i = 0; i < 200; i++) {
        if (XCheckWindowEvent(g_dpy, g_timeWin, PropertyChangeMask, &ev))
            return ev.xproperty.time;
        usleep(500);
    }
    return CurrentTime;
}

// ICCCM: how a window manager tells a client "you have the focus now, activate
// yourself". Wine answers it with SetForegroundWindow, which is what makes the
// game re-apply its cursor clip.
void sendTakeFocus(::Window w) {
    Atom wmProtocols = XInternAtom(g_dpy, "WM_PROTOCOLS",  False);
    Atom wmTakeFocus = XInternAtom(g_dpy, "WM_TAKE_FOCUS", False);
    Atom* protos = nullptr; int n = 0;
    if (!XGetWMProtocols(g_dpy, w, &protos, &n)) return;
    bool supported = false;
    for (int i = 0; i < n; i++) if (protos[i] == wmTakeFocus) supported = true;
    if (protos) XFree(protos);
    if (!supported) return;
    XEvent ev = {};
    ev.type                 = ClientMessage;
    ev.xclient.window       = w;
    ev.xclient.message_type = wmProtocols;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)wmTakeFocus;
    ev.xclient.data.l[1]    = (long)serverTime();
    XSendEvent(g_dpy, w, False, NoEventMask, &ev);
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
    g_timeWin = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy),
                                    -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(g_dpy, g_timeWin, PropertyChangeMask);
    return true;
}

void shutdown() {
    if (!g_dpy) return;
    if (g_timeWin) { XDestroyWindow(g_dpy, g_timeWin); g_timeWin = 0; }
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
    g_win = findByPid(getpid(), false);
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
    // The window is created hidden precisely so this can happen before it is
    // ever shown. A window that is mapped even briefly as an ordinary one gets
    // activated by the window manager, and that deactivates the game, which
    // then drops the pointer it grabbed for mouse-look.
    Atom wmWindowType    = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom wmWindowTypeTip = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    XChangeProperty(g_dpy, g_win, wmWindowType, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&wmWindowTypeTip, 1);

    XSetWindowAttributes ora{};
    ora.override_redirect = 1;
    XChangeWindowAttributes(g_dpy, g_win, CWOverrideRedirect, &ora);

    // Click-through BEFORE the first map, never after. Confining the cursor is
    // a compositor pointer constraint, and a constraint only lives while its
    // surface holds pointer focus. A window that appears over the game with a
    // full input region takes that focus for the moment before its shape is
    // applied, which is enough to cancel the game's confinement -- and the game
    // does not ask for it again until it is next activated. Emptying the input
    // region first means the overlay is never a candidate for pointer focus at
    // all, so the game never notices it.
    setClickThrough(true);

    XMapRaised(g_dpy, g_win);
    XSync(g_dpy, False);

    // Confirm it really is unmanaged; if a window manager owns it, positioning
    // will be off and it is worth saying so out loud.
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

// Is `w` the window `root_of` or nested inside it? The focus often lands on an
// inner drawable rather than the top-level client, which still counts.
bool focusInside(::Window w, ::Window container) {
    for (int depth = 0; w && depth < 32; depth++) {
        if (w == container) return true;
        ::Window root = 0, parent = 0, *kids = nullptr; unsigned nk = 0;
        if (!XQueryTree(g_dpy, w, &root, &parent, &kids, &nk)) return false;
        if (kids) XFree(kids);
        if (parent == root) return false;
        w = parent;
    }
    return false;
}

void focusGame(pid_t gamePid) {
    if (!g_dpy) return;
    ::Window gw = findByPid(gamePid);
    if (!gw) return;

    Atom active = XInternAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);

    // Unmapping the overlay makes the window manager pick a new focus window on
    // its own, and that decision can land after this request. Ask, check, and
    // ask again until it sticks rather than firing once and hoping.
    for (int attempt = 0; attempt < 15; attempt++) {
        // _NET_ACTIVE_WINDOW is the request a window manager acts on: it
        // activates the window the way a click would, which is the event Wine
        // answers by re-grabbing and re-clipping the cursor. Source indication
        // 2 marks it as coming from a pager, so focus-stealing prevention does
        // not drop it.
        XEvent ev = {};
        ev.type                 = ClientMessage;
        ev.xclient.window       = gw;
        ev.xclient.message_type = active;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 2;
        ev.xclient.data.l[1]    = CurrentTime;
        ev.xclient.data.l[2]    = 0;
        XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &ev);

        // Direct route as well, for the case where no window manager answers.
        XWindowAttributes at{};
        if (XGetWindowAttributes(g_dpy, gw, &at) && at.map_state == IsViewable)
            XSetInputFocus(g_dpy, gw, RevertToPointerRoot, CurrentTime);
        sendTakeFocus(gw);
        XSync(g_dpy, False);

        ::Window focus = 0; int revert = 0;
        XGetInputFocus(g_dpy, &focus, &revert);
        if (focusInside(focus, gw)) return;
        usleep(4000);
    }
    printf("[x11] could not hand focus back to the game; "
           "click the game window to give it the mouse\n");
}

void cycleGameWindow(pid_t gamePid) {
    if (!g_dpy) return;
    ::Window gw = findByPid(gamePid);
    if (!gw) {
        printf("[x11] no game window to cycle\n");
        return;
    }
    printf("[x11] minimising and restoring the game so it re-takes the cursor\n");
    fflush(stdout);

    XIconifyWindow(g_dpy, gw, DefaultScreen(g_dpy));
    XFlush(g_dpy);
    usleep(500000);

    XMapRaised(g_dpy, gw);
    XSync(g_dpy, False);
    usleep(300000);

    // Activation, not merely focus: the game re-applies its clip on the former.
    focusGame(gamePid);
    usleep(150000);
    printf("[x11] game restored\n");
    fflush(stdout);
}

bool menuAdopt(const char* title) {
    if (!g_dpy) return false;
    g_win = findByPid(getpid(), false);
    if (!g_win) {
        printf("[menu] could not find the menu window in the tree\n");
        return false;
    }
    if (title) XStoreName(g_dpy, g_win, title);
    return true;
}

void menuSetVisible(bool on) {
    if (!g_dpy || !g_win) return;
    if (!on) { XUnmapWindow(g_dpy, g_win); XFlush(g_dpy); return; }

    XMapRaised(g_dpy, g_win);
    XSync(g_dpy, False);

    // Ask the window manager to activate it, the same request a taskbar makes.
    // Source indication 2 marks it as coming from a pager so focus-stealing
    // prevention does not drop it.
    Atom active = XInternAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);
    XEvent ev = {};
    ev.type                 = ClientMessage;
    ev.xclient.window       = g_win;
    ev.xclient.message_type = active;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 2;
    ev.xclient.data.l[1]    = CurrentTime;
    ev.xclient.data.l[2]    = 0;
    XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
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

bool pointer(int& x, int& y, bool& lmb, bool& rmb) {
    if (!g_dpy || !g_win) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_dpy, g_win, &r, &c, &rx, &ry, &wx, &wy, &mask))
        return false;
    // A window caught between an unmap and the window manager placing it can
    // report a nonsense offset; anything this far out is not a real cursor.
    if (wx < -8192 || wx > 8192 || wy < -8192 || wy > 8192) return false;
    x = wx; y = wy;
    lmb = (mask & Button1Mask) != 0;
    rmb = (mask & Button3Mask) != 0;
    return true;
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
