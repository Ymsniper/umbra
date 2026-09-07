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
#include "x11_overlay.hpp"
#include "shared.hpp"
#include "font.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

// Each of these runs as its own process, with its own ordinary window.
int menuMain(SharedState* sh);
int sonarMain(SharedState* sh);

// Clean shutdown on Ctrl-C
static void sigHandler(int) { g_running = false; }

// Config
static constexpr const char*    kUmbraVersion = "1.4.0";
static constexpr const char*    kProcName   = "Discovery-d.exe";
static constexpr const char*    kModuleName = "Discovery-d.exe";
static constexpr int            kWindowW    = 1920;
static constexpr int            kWindowH    = 1080;

// parse --pid <N> or env GAME_PID
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

// Everything the menu shows that is not a setting it can edit itself.
static void publishStatus(SharedState* sh) {
    Status st;
    st.entityCount     = g_entityCount;
    st.aimTargetCnt    = g_aimTargetCnt;
    st.visVisibleCnt   = g_visVisibleCnt;
    st.visHiddenCnt    = g_visHiddenCnt;
    st.fps             = GetFPS();
    st.trigOnTargetPx  = g_trigOnTargetPx;
    st.trigOnTargetTol = g_trigOnTargetTol;
    st.camFov          = g_camView.FOV;
    st.kmodOk          = g_mem.kmodOk;
    st.kmodFellBack    = g_mem.kmodFellBack;
    st.aimHeld         = g_aimHeld       ? 1 : 0;
    st.aimSuppressed   = g_aimSuppressed ? 1 : 0;
    st.trigHeld        = g_trigHeld      ? 1 : 0;
    st.trigOnTarget    = g_trigOnTarget  ? 1 : 0;
    st.visHave         = g_visHave       ? 1 : 0;
    st.haveLastRenderTime = g_off.Mesh_LastRenderTime ? 1 : 0;
    st.usingKmod       = g_mem.usingKmod()      ? 1 : 0;
    st.vmouseReady     = g_vmouse.ready()       ? 1 : 0;
    st.vmouseKernel    = g_vmouse.usingKernel() ? 1 : 0;
    sh->status = st;
    sh->statusGen.fetch_add(1, std::memory_order_release);
}

// Enemy positions relative to the local player, plus which way it is facing.
// Squadmates are deliberately absent: the sonar is for what you cannot see.
static void publishSonar(SharedState* sh) {
    SonarFrame f;
    {
        std::lock_guard<std::mutex> lk(g_camMtx);
        f.yaw = (float)g_camView.Rotation.Yaw;
    }
    FVector me;
    {
        std::lock_guard<std::mutex> lk(g_camMtx);
        me = g_camView.Location;
    }
    {
        std::lock_guard<std::mutex> lk(g_entityMtx);
        for (int i = 0; i < g_entityCount && f.count < kSharedBlips; i++) {
            const EntityData& e = g_entities[i];
            if (!e.valid || e.isSelf || e.isTeammate) continue;
            SonarBlip& b = f.blips[f.count++];
            b.dx    = (float)(e.origin.X - me.X);
            b.dy    = (float)(e.origin.Y - me.Y);
            b.squad = e.squadIdx;
            b.cls   = classFromMaxHealth(e.maxHealth);
        }
    }
    sh->sonar = f;
    sh->sonarGen.fetch_add(1, std::memory_order_release);
}

int main(int argc, char* argv[]) {
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

    // 1. Attach to process
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

    // The menu is a separate process with an ordinary window. Its mapping is
    // created here, before the fork, so both sides inherit it and there is no
    // named object to clean up. Forking before any X or GL call keeps each
    // process's graphics state entirely its own.
    SharedState* shared = sharedCreate();
    SettingsMirror settings;
    pid_t menuPid = -1, sonarPid = -1;
    if (!shared) {
        printf("[main] shared mapping failed; running without the menu\n");
    } else {
        shared->gamePid = (int32_t)g_mem.pid;
        sharedWriteFields(shared);
        settings.gen = shared->settingsGen.load();
        mirrorCapture(settings);
        menuPid = fork();
        if (menuPid == 0) _exit(menuMain(shared));
        if (menuPid < 0)
            printf("[main] could not start the menu process (%s)\n", strerror(errno));

        sonarPid = fork();
        if (sonarPid == 0) _exit(sonarMain(shared));
        if (sonarPid < 0)
            printf("[main] could not start the sonar process (%s)\n", strerror(errno));
    }

    // Virtual pointer for aim assist. A SEPARATE device, so the real mouse keeps
    // working and its movement sums with ours - the assist can be fought.
    uintptr_t GWorld = 0;          // unused at runtime; reader ignores it
    g_vmouse.open();

    // 2. Transparent overlay window
    const bool haveX11 = ovl::init();

    int ox = 0, oy = 0, ow = kWindowW, oh = kWindowH;
    if (haveX11) {
        if (ovl::gameRect(g_mem.pid, ox, oy, ow, oh))
            printf("[x11] game window at %d,%d %dx%d - overlay will match\n",
                   ox, oy, ow, oh);
        else
            printf("[x11] WARNING: game window not found; overlay at 0,0 %dx%d\n",
                   ow, oh);
    }

    // FLAG_MSAA_4X_HINT is deliberately absent. Asking for multisampling makes
    // GLFW pick a multisample framebuffer config, and none of those carry an
    // alpha channel here: the window comes back at depth 24 instead of 32 and
    // the overlay is an opaque black rectangle over the game.
    // HIDDEN matters as much as the rest: a window that is mapped even for an
    // instant as an ordinary one is activated by the window manager, and that
    // deactivates the game and makes it release the cursor. It is mapped in
    // adoptOwnWindow, once it is already override-redirect and invisible to the
    // window manager. ALWAYS_RUN keeps WindowShouldClose from blocking if the
    // window is ever taken for minimized.
    SetConfigFlags(FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED |
                   FLAG_WINDOW_TOPMOST | FLAG_WINDOW_MOUSE_PASSTHROUGH |
                   FLAG_WINDOW_HIDDEN | FLAG_WINDOW_ALWAYS_RUN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(ow, oh, "TheFinals");
    if (!IsWindowReady()) {
        printf("[main] could not create the overlay window\n");
        return 1;
    }
    // End quits the overlay; raylib would otherwise also quit on ESC, which is
    // the game's own menu key.
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    if (haveX11) {
        ovl::adoptOwnWindow("TheFinals");
        ovl::moveResize(ox, oy, ow, oh);
        ovl::focusGame(g_mem.pid);
    }

    // raylib's BLEND_ALPHA uses the source alpha as the destination factor on
    // the alpha channel too, so a 67%-opaque box lands in the framebuffer at
    // 44% and the overlay reads washed out over the game. Keeping the colour
    // factors and using GL_ONE for source alpha composites to the value the
    // opacity slider actually asks for.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                              RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);

    // 3. Font
    Font font = loadUiFont();

    // 4. Start reader thread
    std::thread reader(readerThread, GWorld);

    // 5. Hand the cursor back to the game. Set UMBRA_NO_CYCLE=1 to skip it.
    if (haveX11 && !getenv("UMBRA_NO_CYCLE"))
        ovl::cycleGameWindow(g_mem.pid);

    // 6. Main render loop
    unsigned long frames = 0;
    while (!WindowShouldClose() && g_running) {
        ++frames;

        if (haveX11) {
            // Follow the game window. Half a second between checks is plenty
            // for an alt-tab or a resolution change and costs nothing.
            static int geomTick = 0;
            if ((geomTick++ % 30) == 0) {
                int gx = 0, gy = 0, gw = 0, gh = 0;
                if (ovl::gameRect(g_mem.pid, gx, gy, gw, gh)) {
                    static int lx = -1, ly = -1, lw = 0, lh = 0;
                    // SetWindowSize keeps raylib's own screen size and GL
                    // viewport in step, which the projection depends on, so it
                    // only runs when the size really changed.
                    if (gw != lw || gh != lh) SetWindowSize(gw, gh);
                    if (gx != lx || gy != ly || gw != lw || gh != lh) {
                        printf("[x11] overlay -> %d,%d %dx%d (game window)\n",
                               gx, gy, gw, gh);
                        lx = gx; ly = gy; lw = gw; lh = gh;
                    }
                    // Re-asserted every tick, not only when the game moves: a
                    // window manager releasing a window it was managing puts it
                    // back where it had it about a second later, and any drift
                    // offsets the entire ESP by exactly that much.
                    ovl::moveResize(gx, gy, gw, gh);
                }
            }
            ovl::raise();
            // End quits. Polled rather than read from the window's event queue,
            // which only ever fills while the menu holds the pointer grab.
            if (ovl::keyDown(ovl::KeyEnd)) g_running = false;

            // HOME toggles aim assist; the selected mouse button is the fire
            // condition.
            static bool prevHome = false;
            const bool nowHome = ovl::keyDown(ovl::KeyHome);
            if (nowHome && !prevHome) {
                g_aimEnabled = !g_aimEnabled;
                printf("[aim] %s (HOME)\n", g_aimEnabled ? "ENABLED" : "disabled");
            }
            prevHome = nowHome;

            const bool lmb = ovl::lmbDown();
            const bool rmb = ovl::rmbDown();
            g_lmbHeld  = lmb;
            g_aimHeld  = (g_aimButton == 1) ? lmb
                       : (g_aimButton == 2) ? (lmb || rmb)
                                            : rmb;           // RMB = ADS default
            // The triggerbot takes RMB held or no button at all, deliberately
            // fewer choices than the aimbot.
            g_trigHeld = (g_trigButton == 1) ? true : rmb;

            // Quick scope. The shot releases the aim; it comes back on the next
            // ADS press, or after the restore delay if one is set.
            {
                static bool prevLmb = false, prevAim = false;
                static std::chrono::steady_clock::time_point firedAt;
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

            // INSERT belongs to the menu process; this window never takes
            // focus and never stops being click-through.
        }

        if (shared) {
            // Takes what the menu changed, publishes what HOME changed here.
            sharedSync(shared, settings);
            publishStatus(shared);
            if (g_sonarEnabled) publishSonar(shared);
        }

        BeginDrawing();
            ClearBackground(BLANK);               // transparent, not black
            BeginBlendMode(BLEND_CUSTOM_SEPARATE);
                renderFrame(font);
            EndBlendMode();
        EndDrawing();
    }

    printf("[loop] exited after %lu frames\n", frames);

    // 7. Cleanup
    g_running = false;
    if (reader.joinable()) reader.join();
    if (shared) {
        // Let each publish any last edit and go. Never an unbounded wait: a
        // child that does not answer must not keep the overlay alive.
        const pid_t children[2] = { menuPid, sonarPid };
        for (pid_t child : children) {
            if (child <= 0) continue;
            kill(child, SIGTERM);
            bool reaped = false;
            for (int i = 0; i < 200 && !reaped; i++) {
                if (waitpid(child, nullptr, WNOHANG) == child) reaped = true;
                else usleep(5000);
            }
            if (!reaped) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        }
        // The children's settings live in the mapping, so they survive them
        // exiting; take the final state before it is written to disk.
        sharedReadFields(shared);
    }
    saveSettings();          // come back next launch the way the user left it
    if (font.texture.id != GetFontDefault().texture.id) UnloadFont(font);
    CloseWindow();
    ovl::shutdown();

    if (g_mem.usingKmod())
        printf("[mem] kernel backend: %llu reads via module, %llu fell back to pvr\n",
               (unsigned long long)g_mem.kmodOk,
               (unsigned long long)g_mem.kmodFellBack);
    else
        printf("[mem] backend was process_vm_readv the whole run (no module)\n");

    printf("[main] clean exit.\n");
    return 0;
}
