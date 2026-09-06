// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Entry point: attach to the game, create the click-through overlay
// window, start the reader thread and run the render loop.
#include "mem.hpp"
#include "cheat.hpp"
#include "render.hpp"
#include "global.hpp"
#include "vmouse.hpp"
#include "runtime_offsets.hpp"
#include "settings.hpp"

#include <SFML/Graphics.hpp>
#include <imgui.h>
#include <imgui-SFML.h>
#include <cstdio>
#include <thread>
#include <csignal>
#include <optional>
#include <vector>

#ifdef HAVE_X11
#  include <X11/Xlib.h>
#  include <X11/Xatom.h>
#  include <X11/keysym.h>
#  include <X11/XKBlib.h>
#  ifdef HAVE_XSHAPE
#    include <X11/extensions/shape.h>
#  endif

// x11SetupOverlay
static Display*  g_ovlDpy = nullptr;   // used ONLY by the main/render thread
static Display*  g_hotDpy = nullptr;   // separate connection for hotkeys + shape
static ::Window  g_ovlWin = 0;
static bool      g_interactive = false;
// Where the overlay was created (the game window's rect), so nothing later
// repositions it blindly.
static struct { int x = 0, y = 0; unsigned w = 0, h = 0; } g_ovlRect;

static int x11IgnoreError(Display* d, XErrorEvent* e) {
    char buf[128] = {0};
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    printf("[x11] (ignored) %s  request %d.%d\n",
           buf, e->request_code, e->minor_code);
    return 0;
}

static void x11GrabInput(bool grab) {
    if (!g_hotDpy || !g_ovlWin) return;
    if (grab) {
        int r = XGrabPointer(g_hotDpy, g_ovlWin, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, 0L, 0L, CurrentTime);
        const char* why = r == 0 ? "ok"
                        : r == 1 ? "AlreadyGrabbed (game holds it)"
                        : r == 2 ? "InvalidTime" : r == 3 ? "NotViewable" : "Frozen";
        printf("[x11] grab pointer: %s\n", why);
        XGrabKeyboard(g_hotDpy, g_ovlWin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    } else {
        XUngrabPointer(g_hotDpy, CurrentTime);
        XUngrabKeyboard(g_hotDpy, CurrentTime);
        printf("[x11] pointer/keyboard released back to the game\n");
    }
    XFlush(g_hotDpy);
}

static void x11SetClickThrough(bool through) {
    if (!g_hotDpy || !g_ovlWin) return;
    #ifdef HAVE_XSHAPE
    if (through) {
        Region empty = XCreateRegion();
        XShapeCombineRegion(g_hotDpy, g_ovlWin, ShapeInput, 0, 0, empty, ShapeSet);
        XDestroyRegion(empty);
    } else {
        // 0L = None: reset the input shape to the full window.
        XShapeCombineMask(g_hotDpy, g_ovlWin, ShapeInput, 0, 0, 0L, ShapeSet);
    }
    XFlush(g_hotDpy);
    #endif
}


// Left mouse button, polled globally. The overlay never has focus, so SFML's
// event queue never sees the game's clicks - query the pointer directly.
static bool x11AimButtonDown() {
    if (!g_hotDpy) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_hotDpy, DefaultRootWindow(g_hotDpy),
                       &r, &c, &rx, &ry, &wx, &wy, &mask)) return false;
    bool lmb = (mask & Button1Mask) != 0;
    bool rmb = (mask & Button3Mask) != 0;   // Button2 is middle, not right
    switch (g_aimButton) {
        case 1:  return lmb;
        case 2:  return lmb || rmb;
        default: return rmb;                // RMB = ADS, the sane default
    }
}

// The fire button, read on its own: quick scope needs the shot itself, not
// whichever button happens to be driving the aim.
static bool x11LmbDown() {
    if (!g_hotDpy) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_hotDpy, DefaultRootWindow(g_hotDpy),
                       &r, &c, &rx, &ry, &wx, &wy, &mask)) return false;
    return (mask & Button1Mask) != 0;
}

// Triggerbot button: RMB held, or always-on (no button). Deliberately smaller
// than the aimbot's choices -- the user asked for exactly "rmb held or nothing".
static bool x11TrigButtonDown() {
    if (g_trigButton == 1) return true;      // always active, no button
    if (!g_hotDpy) return false;
    ::Window r, c; int rx, ry, wx, wy; unsigned mask = 0;
    if (!XQueryPointer(g_hotDpy, DefaultRootWindow(g_hotDpy),
                       &r, &c, &rx, &ry, &wx, &wy, &mask)) return false;
    return (mask & Button3Mask) != 0;        // RMB
}

static bool x11KeyDown(KeySym ks) {
    if (!g_hotDpy) return false;
    char keys[32];
    XQueryKeymap(g_hotDpy, keys);
    KeyCode kc = XKeysymToKeycode(g_hotDpy, ks);
    if (!kc) return false;
    return (keys[kc / 8] & (1 << (kc % 8))) != 0;
}

// ── follow the game window
static ::Window x11FindGameWindow(Display* dpy, pid_t gamePid) {
    Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (!pidAtom) return 0;
    ::Window best = 0;
    unsigned bestArea = 0;

    // Breadth-first over the window tree; the game may nest its drawable.
    std::vector< ::Window> queue{ DefaultRootWindow(dpy) };
    for (size_t qi = 0; qi < queue.size() && qi < 4096; qi++) {
        ::Window w = queue[qi];
        ::Window root = 0, parent = 0, *kids = nullptr;
        unsigned nkids = 0;
        if (XQueryTree(dpy, w, &root, &parent, &kids, &nkids)) {
            for (unsigned i = 0; i < nkids; i++) queue.push_back(kids[i]);
            if (kids) XFree(kids);
        }
        Atom type = 0; int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False, XA_CARDINAL,
                               &type, &fmt, &nitems, &after, &data) != Success)
            continue;
        if (!data) continue;
        pid_t wpid = (pid_t)(*(unsigned long*)data);
        XFree(data);
        if (gamePid > 0 && wpid != gamePid) continue;

        XWindowAttributes at{};
        if (!XGetWindowAttributes(dpy, w, &at)) continue;
        if (at.map_state != IsViewable) continue;
        unsigned area = (unsigned)at.width * (unsigned)at.height;
        if (area > bestArea) { bestArea = area; best = w; }
    }
    return best;
}

// Absolute screen rect of a window (XWindowAttributes x/y are parent-relative).
static bool x11WindowRect(Display* dpy, ::Window w, int& x, int& y,
                          unsigned& cw, unsigned& ch) {
    XWindowAttributes at{};
    if (!XGetWindowAttributes(dpy, w, &at)) return false;
    ::Window child = 0;
    if (!XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &x, &y, &child))
        return false;
    cw = (unsigned)at.width;
    ch = (unsigned)at.height;
    return cw > 16 && ch > 16;
}

static void x11SetupOverlay(unsigned long nativeHandle) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        printf("[x11] XOpenDisplay failed - overlay hints skipped\n");
        printf("[x11] Run with: DISPLAY=:0 WAYLAND_DISPLAY=\"\" ./TheFinals\n");
        return;
    }

    ::Window xwin = (::Window)nativeHandle;

    #ifdef HAVE_XSHAPE
    // 1. Click-through: empty ShapeInput region - mouse clicks pass through
    //    the overlay and reach the game window underneath.
    Region empty = XCreateRegion();
    XShapeCombineRegion(dpy, xwin, ShapeInput, 0, 0, empty, ShapeSet);
    XDestroyRegion(empty);
    printf("[x11] click-through enabled (ShapeInput)\n");
    #else
    printf("[x11] WARNING: XShape not available - overlay will BLOCK mouse clicks\n");
    printf("[x11]          Arch: sudo pacman -S libxext   then rebuild\n");
    #endif

    // 2. Always-on-top: ClientMessage to root (correct method for mapped windows)
    Atom wmState      = XInternAtom(dpy, "_NET_WM_STATE",       False);
    Atom wmStateAbove = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XEvent ev = {};
    ev.type                 = ClientMessage;
    ev.xclient.window       = xwin;
    ev.xclient.message_type = wmState;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 1;                  // _NET_WM_STATE_ADD
    ev.xclient.data.l[1]    = (long)wmStateAbove;
    ev.xclient.data.l[2]    = 0;
    ev.xclient.data.l[3]    = 1;                  // source: application
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    printf("[x11] always-on-top requested (_NET_WM_STATE_ABOVE)\n");

    Atom wmWindowType    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom wmWindowTypeTip = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    XChangeProperty(dpy, xwin, wmWindowType, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&wmWindowTypeTip, 1);
    XSetWindowAttributes ora{};
    ora.override_redirect = 1;
    XChangeWindowAttributes(dpy, xwin, CWOverrideRedirect, &ora);
    printf("[x11] window type TOOLTIP + override-redirect (no taskbar, no WM stacking)\n");

    XFlush(dpy);
    XCloseDisplay(dpy);
    printf("[x11] overlay setup complete\n");
}

// X11/Xlib.h defines these as macros which clash with SFML identifiers.
// Undef them after the X11 function so the rest of the file compiles cleanly.
#  undef None     // Xlib: #define None 0L  - breaks sf::Style::None
#  undef Bool     // Xlib: #define Bool int
#  undef Status   // Xlib: #define Status int
#  undef True     // Xlib: #define True 1
#  undef False    // Xlib: #define False 0
#endif // HAVE_X11

// Clean shutdown on Ctrl-C
static void sigHandler(int) { g_running = false; }

// Config
static constexpr const char*    kUmbraVersion = "1.0";
static constexpr const char*    kProcName   = "Discovery-d.exe";
static constexpr const char*    kModuleName = "Discovery-d.exe";
static constexpr unsigned int   kWindowW    = 1920;   // unsigned - matches sf::VideoMode
static constexpr unsigned int   kWindowH    = 1080;

// ── parse --pid <N> or env GAME_PID
static pid_t parseForcedPID(int argc, char* argv[]) {
    // 1. Command-line: --pid <N> or -p <N>
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--pid") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            pid_t p = (pid_t)atoi(argv[++i]);
            if (p > 0) return p;
            fprintf(stderr, "[main] invalid --pid value '%s'\n", argv[i]);
            return -1;
        }
    }
    // 2. Environment variable: GAME_PID=12345
    const char* env = getenv("GAME_PID");
    if (env && *env) {
        pid_t p = (pid_t)atoi(env);
        if (p > 0) return p;
    }
    return -1; // not specified - fall back to name search
}

int main(int argc, char* argv[]) {
    #ifdef HAVE_X11
    XInitThreads();
    #endif
    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGPIPE, SIG_IGN);

    printf("=== Umbra %s - external overlay for THE FINALS (Linux) ===\n"
           "    Copyright (C) 2026 Ymsniper. GPLv2, no warranty.\n\n",
           kUmbraVersion);
    if (!loadOffsets()) {
        printf("[offsets] offsets.cfg not found next to the binary or one level up.\n"
               "          It carries every game offset; the tool cannot run without it.\n");
        return 1;
    }
    if (!offsetsSane()) {
        printf("[offsets] offsets.cfg is present but incomplete (see above).\n"
               "          Re-derive it with the updater and copy it back here.\n");
        return 1;
    }
    loadSettings();   // menu preferences from the last run

    // ── 1. Attach to process
    pid_t forced_pid = parseForcedPID(argc, argv);
    auto tryAttach = [&]() -> bool {
        if (forced_pid > 0)
            return g_mem.initByPID(forced_pid, kModuleName);
        return g_mem.init(kProcName, kModuleName);
    };

    if (!tryAttach()) {
        if (forced_pid > 0) {
            printf("[main] PID %d: module '%s' not found in maps.\n", forced_pid, kModuleName);
            printf("[main] Is the game running and is the PID correct?\n");
            return 1;
        }
        printf("[main] Waiting for %s ...\n", kProcName);
        printf("[main] Under Proton/Wine, use --pid or GAME_PID= if this loops forever.\n");
        printf("[main]   grep -rl '%s' /proc/*/maps 2>/dev/null | head -5\n", kModuleName);
        while (g_running && !tryAttach())
            std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_running) return 1;
    }

    // Virtual pointer for aim assist. A SEPARATE device, so the real mouse keeps
    // working and its movement sums with ours - the assist can be fought.
    uintptr_t GWorld = 0;          // unused at runtime; reader ignores it
    g_vmouse.open();

    // ── 3. Create transparent overlay window
    std::optional<sf::RenderWindow> windowOpt;
    #ifdef HAVE_X11
    static Display* g_overlayDpy = nullptr;
    {
        XSetErrorHandler(x11IgnoreError);
        Display* dpy = XOpenDisplay(nullptr);
        XVisualInfo vinfo{};
        if (dpy && XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
            ::Window root = DefaultRootWindow(dpy);
            XSetWindowAttributes attrs{};
            attrs.colormap        = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
            attrs.border_pixel    = 0;
            attrs.background_pixel = 0;          // fully transparent, not black
            attrs.override_redirect = 0;
            int ox = 0, oy = 0;
            unsigned ow = kWindowW, oh = kWindowH;
            {
                ::Window gw = x11FindGameWindow(dpy, g_mem.pid);
                int gx, gy; unsigned gcw, gch;
                if (gw && x11WindowRect(dpy, gw, gx, gy, gcw, gch)) {
                    ox = gx; oy = gy; ow = gcw; oh = gch;
                    g_ovlRect = {gx, gy, gcw, gch};
                    printf("[x11] game window at %d,%d %ux%u - overlay will match\n",
                           gx, gy, gcw, gch);
                } else {
                    printf("[x11] WARNING: game window not found; overlay at 0,0 %ux%u\n",
                           ow, oh);
                }
            }
            ::Window w = XCreateWindow(dpy, root, ox, oy, ow, oh, 0,
                                     vinfo.depth, InputOutput, vinfo.visual,
                                     CWColormap | CWBorderPixel | CWBackPixel |
                                     CWOverrideRedirect, &attrs);
            if (w) {
                XStoreName(dpy, w, "TheFinals");
                XSizeHints hints{};
                hints.flags  = USPosition | USSize | PPosition | PSize;
                hints.x      = ox;      hints.y      = oy;
                hints.width  = (int)ow; hints.height = (int)oh;
                XSetWMNormalHints(dpy, w, &hints);
                XMapWindow(dpy, w);
                // Re-assert after mapping: some WMs still reposition on map.
                XMoveResizeWindow(dpy, w, ox, oy, ow, oh);
                XFlush(dpy);
                printf("[x11] ARGB overlay: 32-bit visual 0x%lx, window 0x%lx\n",
                       vinfo.visualid, w);
                windowOpt.emplace(static_cast<sf::WindowHandle>(w));
                g_overlayDpy = dpy;      // keep alive; do NOT close
                g_ovlDpy = dpy;
                g_ovlWin = w;
                g_hotDpy = XOpenDisplay(nullptr);   // independent connection
            }
        } else {
            printf("[x11] WARNING: no 32-bit ARGB visual - overlay will be OPAQUE\n"
                   "      and will black out the screen. Press End to quit if that happens.\n");
        }
        if (dpy && !g_overlayDpy) XCloseDisplay(dpy);
    }
    #endif

    if (!windowOpt) {
        windowOpt.emplace(sf::VideoMode({kWindowW, kWindowH}),
                          "TheFinals",
                          sf::Style::None);
    }
    sf::RenderWindow& window = *windowOpt;
    window.setFramerateLimit(60);
    // NOTE: do NOT setPosition({0,0}) here. The ARGB window above is already
    // created over the game window, and this line (left over from the original
    // single-monitor code) dragged it straight back to the primary screen,
    // silently undoing every positioning fix.
    if (g_ovlRect.w) window.setPosition({g_ovlRect.x, g_ovlRect.y});

    window.clear(sf::Color(0, 0, 0, 0));

    #ifdef HAVE_X11
    // Apply click-through, always-on-top, and DOCK window type.
    // Must run AFTER window construction (window must be mapped first).
    // If this prints "XOpenDisplay failed", relaunch with:
    //   DISPLAY=:0 WAYLAND_DISPLAY="" ./TheFinals --pid <PID>
    x11SetupOverlay(window.getNativeHandle());
    #endif

    // SFML 3: ImGui::SFML::Init now returns bool - check it
    if (!ImGui::SFML::Init(window)) {
        printf("[main] ImGui::SFML::Init failed\n");
        return 1;
    }
    ImGui::GetStyle().Alpha = 0.9f;

    // ── 4. Load font
    sf::Font font;
    // Arch first, Debian second. The old order printed SFML's "Failed to load
    // font" to stderr on every start even though the fallback worked, which
    // reads like a fatal error and is not one.
    const char* kFonts[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                    // Arch
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",        // Debian/Ubuntu
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    };
    bool fontOk = false;
    for (const char* f : kFonts)
        if ((fontOk = font.openFromFile(f))) break;
    if (!fontOk) printf("[main] WARNING: could not load DejaVuSans.ttf; text labels disabled.\n");

    // ── 5. Start reader thread
    std::thread reader;
    reader = std::thread(readerThread, GWorld);

    // ── 6. Main render loop
    sf::Clock imguiClock;

    unsigned long frames = 0;
    while (window.isOpen() && g_running) {
        ++frames;
        // SFML 3: event loop uses std::optional<sf::Event> - no sentinel type
        while (const auto event = window.pollEvent()) {
            // SFML 3: ProcessEvent takes (window, event) instead of just (event)
            ImGui::SFML::ProcessEvent(window, *event);

            if (event->is<sf::Event::Closed>())
                g_running = false;

            // SFML 3: key events use scancode, not keycode
            if (const auto* kp = event->getIf<sf::Event::KeyPressed>())
                if (kp->scancode == sf::Keyboard::Scan::End)
                    g_running = false;
        }

        #ifdef HAVE_X11
        if (g_hotDpy && g_ovlWin) {
            static int geomTick = 0;
            if ((geomTick++ % 30) == 0) {
                static ::Window gameWin = 0;
                if (!gameWin) gameWin = x11FindGameWindow(g_hotDpy, g_mem.pid);
                int gx = 0, gy = 0; unsigned gw = 0, gh = 0;
                if (gameWin && x11WindowRect(g_hotDpy, gameWin, gx, gy, gw, gh)) {
                    static int lx = -1, ly = -1; static unsigned lw = 0, lh = 0;
                    if (gx != lx || gy != ly || gw != lw || gh != lh) {
                        // Xlib directly: SFML will not move a window it does not own.
                        XMoveResizeWindow(g_hotDpy, g_ovlWin, gx, gy, gw, gh);
                        XFlush(g_hotDpy);
                        // Keep the GL view matching, or the projection uses a
                        // stale viewport and boxes land in the wrong place.
                        window.setView(sf::View(sf::FloatRect({0.f, 0.f},
                                                {(float)gw, (float)gh})));
                        printf("[x11] overlay -> %d,%d %ux%u (game window)\n", gx, gy, gw, gh);
                        lx = gx; ly = gy; lw = gw; lh = gh;
                    }
                }
            }
            XRaiseWindow(g_hotDpy, g_ovlWin);
            XFlush(g_hotDpy);
        }

        // HOME toggles aim assist; the selected mouse button is the fire condition.
        {
            static bool prevHome = false;
            bool nowHome = x11KeyDown(XK_Home);
            if (nowHome && !prevHome) {
                g_aimEnabled = !g_aimEnabled;
                printf("[aim] %s (HOME)\n", g_aimEnabled ? "ENABLED" : "disabled");
            }
            prevHome = nowHome;
            g_aimHeld = x11AimButtonDown();
            g_trigHeld = x11TrigButtonDown();

            // Quick scope. The shot releases the aim; it comes back on the next
            // ADS press, or after the restore delay if one is set.
            {
                static bool prevLmb = false, prevAim = false;
                static std::chrono::steady_clock::time_point firedAt;
                const bool lmb = x11LmbDown();
                g_lmbHeld = lmb;
                // aim and fire must be different buttons; see the menu note
                if (g_aimQuickScope && g_aimButton == 0) {
                    if (lmb && !prevLmb && g_aimHeld) {
                        g_aimSuppressed = true;
                        firedAt = std::chrono::steady_clock::now();
                    }
                    if (g_aimSuppressed) {
                        if (g_aimHeld && !prevAim) {
                            g_aimSuppressed = false;      // fresh ADS press
                        } else if (g_aimQuickRestoreMs > 0) {
                            const auto ms = std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - firedAt).count();
                            if (ms >= g_aimQuickRestoreMs) g_aimSuppressed = false;
                        }
                    }
                } else {
                    g_aimSuppressed = false;
                }
                prevLmb = lmb; prevAim = g_aimHeld;
            }
        }

        // INSERT toggles menu interaction. Click-through is what makes the
        // overlay usable in-game, but it also means the ImGui menu can never
        // be clicked - there was no way to reach the settings at all before.
        {
            static bool prevInsert = false;
            bool nowInsert = x11KeyDown(XK_Insert);
            if (nowInsert && !prevInsert) {
                g_interactive = !g_interactive;
                g_menuVisible.store(g_interactive);
                x11SetClickThrough(!g_interactive);
                x11GrabInput(g_interactive);
                printf("[x11] menu %s  (triggered by keycode %d)\n",
                       g_interactive ? "shown + INTERACTIVE - clicks hit the overlay"
                                     : "hidden + click-through - clicks hit the game",
                       (int)XKeysymToKeycode(g_hotDpy, XK_Insert));
            }
            prevInsert = nowInsert;
        }
        #endif

        window.clear(sf::Color(0, 0, 0, 0));   // transparent background
        renderFrame(window, font, imguiClock);
        window.display();
    }

    printf("[loop] exited after %lu frames\n", frames);

    #ifdef HAVE_X11
    // Never exit holding the pointer - that would leave the desktop unusable.
    if (g_interactive) x11GrabInput(false);
    #endif

    // ── 7. Cleanup
    g_running = false;
    if (reader.joinable()) reader.join();
    saveSettings();          // come back next launch the way the user left it
    ImGui::SFML::Shutdown();
    window.close();

    if (g_mem.usingKmod())
        printf("[mem] kernel backend: %llu reads via module, %llu fell back to pvr\n",
               (unsigned long long)g_mem.kmodOk,
               (unsigned long long)g_mem.kmodFellBack);
    else
        printf("[mem] backend was process_vm_readv the whole run (no module)\n");

    printf("[main] clean exit.\n");
    return 0;
}
