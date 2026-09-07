// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// The settings menu, which runs as its own process in its own ordinary window.
// See shared.hpp for why it is not drawn by the overlay.
#include "shared.hpp"
#include "x11_overlay.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <imgui.h>
#include <rlImGui.h>
#include <cstdio>
#include <sys/prctl.h>
#include <unistd.h>
#include <signal.h>

static void drawSettingsPanel(const Status& st) {
    // The panel fills the window; the window itself is what the user moves and
    // resizes, so ImGui does not draw a second set of chrome inside it.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)GetScreenWidth(), (float)GetScreenHeight()));
    // Only the panel's background fades; the text and widgets on it stay solid,
    // which keeps it readable over whatever is behind.
    ImGui::SetNextWindowBgAlpha((float)g_menuAlpha / 255.f);
    ImGui::Begin("Umbra", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header: name, and the two facts worth seeing without opening a tab.
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "UMBRA");
    ImGui::SameLine();
    ImGui::TextDisabled("| %d entities", st.entityCount);
    if (g_aimEnabled) {
        ImGui::SameLine();
        ImGui::TextColored(st.aimSuppressed ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                           : st.aimHeld       ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           st.aimSuppressed ? "| aim released"
                           : st.aimHeld     ? "| AIM" : "| aim");
    }
    if (g_trigEnabled) {
        ImGui::SameLine();
        ImGui::TextColored(st.trigOnTarget ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                          : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           st.trigOnTarget ? "| TRIG" : "| trig");
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("umbra_tabs")) {

    if (ImGui::BeginTabItem("ESP")) {
        ImGui::Checkbox("Boxes",       &g_espBoxes);
        ImGui::Checkbox("Skeleton",    &g_espSkeleton);
        ImGui::Checkbox("Snaplines",   &g_espSnaplines);
        ImGui::Checkbox("Health",      &g_espHealth);
        ImGui::Checkbox("Names",       &g_espName);
        ImGui::Checkbox("Distance",    &g_espDistance);
        ImGui::Checkbox("Team colors", &g_espTeamColor);
        ImGui::Spacing();
        ImGui::Checkbox("Show squadmates", &g_espTeammates);
        ImGui::Checkbox("Show self",       &g_espSelf);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::SliderInt("Opacity",       &g_espAlpha, 30, 255);
        ImGui::SliderFloat("Line width",  &g_espThickness, 1.f, 6.f, "%.1f px");
        ImGui::SliderFloat("Text size",   &g_espTextSize, 8.f, 28.f, "%.0f px");
        ImGui::SliderFloat("Max distance", &g_maxEspDist, 50.f, 1000.f, "%.0f m");
        ImGui::SliderFloat("Box headroom", &g_boxHeadroom, 0.f, 0.20f, "%.3f");
        ImGui::SameLine(); ImGui::TextDisabled("(no skeleton only)");
        ImGui::SliderFloat("Min box height", &g_boxMinPx, 0.f, 60.f, "%.0f px");
        ImGui::SameLine();
        ImGui::TextDisabled(g_boxMinPx <= 0.f ? "(off: true scaling)"
                                              : "(far targets stop shrinking)");
        ImGui::Spacing();
        ImGui::SliderFloat("FOV scale", &g_fovScale, 0.70f, 1.60f, "%.3f");
        ImGui::TextDisabled("game FOV %.1f -> drawn at %.1f",
                            st.camFov, st.camFov * g_fovScale);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Aim")) {
        ImGui::Checkbox("Enabled##aim", &g_aimEnabled);
        ImGui::SameLine();
        if (st.vmouseReady)
            ImGui::TextDisabled("HOME toggles");
        else
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "no mouse backend");

        const char* btns[] = { "Right mouse (ADS)", "Left mouse", "Either" };
        ImGui::Combo("Hold", &g_aimButton, btns, 3);
        const char* bones[] = { "Head", "Chest", "Body", "Legs" };
        ImGui::Combo("Target bone", &g_aimBone, bones, 4);

        ImGui::Separator();
        ImGui::TextDisabled("Motion");
        ImGui::Combo("Smoothing", &g_aimSmoothMode, "divisor\0inertia (EMA)\0");
        ImGui::SliderFloat("Smoothness", &g_aimSmooth, 1.f, 20.f, "%.1f");
        if (g_aimSmoothMode == 1)
            ImGui::SliderFloat("Inertia", &g_aimInertia, 0.f, 1.f, "%.2f");
        ImGui::Checkbox("Predict moving targets", &g_aimPredict);
        if (g_aimPredict)
            ImGui::SliderFloat("Lead (ms)", &g_aimLeadMs, 0.f, 120.f, "%.0f");
        // The curve shapes the path whatever button drives the aim, so it is
        // not tied to the quick-scope button condition below.
        ImGui::Checkbox("Curved pull", &g_aimCurve);
        ImGui::SameLine(); ImGui::TextDisabled("(arc instead of a straight line)");
        if (g_aimCurve) {
            ImGui::SliderFloat("Bow: sideways -> up/down", &g_aimCurveX, 0.5f, 20.f, "%.2f");
            ImGui::SameLine(); ImGui::TextDisabled("(lower = wider)");
            ImGui::SliderFloat("Bow: up/down -> sideways", &g_aimCurveY, 0.5f, 20.f, "%.2f");
            ImGui::SameLine(); ImGui::TextDisabled("(lower = wider)");
            ImGui::Checkbox("Arc above", &g_aimCurveAbove);
            ImGui::SameLine(); ImGui::TextDisabled(g_aimCurveAbove ? "" : "(below)");
            ImGui::SliderFloat("Curve jitter (%)", &g_aimCurveJitter, 0.f, 25.f, "%.1f");
        }

        // Quick scope only means something when the aim is on a different
        // button from the shot. On "Left mouse" or "Either" the trigger IS the
        // aim button, so firing would release and re-arm in the same instant
        // and do nothing at all.
        const bool qsUsable = (g_aimButton == 0);
        if (!qsUsable) ImGui::BeginDisabled();
        ImGui::Checkbox("Quick scope", &g_aimQuickScope);
        ImGui::SameLine();
        ImGui::TextDisabled(qsUsable ? "(let go the moment you fire)"
                                     : "(needs Hold = right mouse)");
        if (g_aimQuickScope && qsUsable) {
            ImGui::SliderInt("Re-arm (ms)", &g_aimQuickRestoreMs, 0, 1500);
            ImGui::SameLine();
            ImGui::TextDisabled(g_aimQuickRestoreMs == 0 ? "(next ADS press)" : "");
        }
        if (!qsUsable) ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled("Target choice");
        ImGui::SliderFloat("Near me <-> crosshair", &g_aimTargetBias, 0.f, 1.f, "%.2f");
        ImGui::Checkbox("Sticky target", &g_aimSticky);
        if (g_aimSticky)
            ImGui::SliderFloat("Stickiness", &g_aimStickiness, 1.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("FOV (px)",     &g_aimFovPx, 20.f, 600.f, "%.0f");
        ImGui::Checkbox("Show FOV circle", &g_aimShowFov);
        ImGui::Checkbox("Scale FOV by distance", &g_aimDistFov);
        ImGui::SliderFloat("Max distance##aim", &g_aimMaxDist, 10.f, 300.f, "%.0f m");

        ImGui::Separator();
        ImGui::TextDisabled("Head offset");
        ImGui::SliderFloat("Forward", &g_aimHeadFwd, -0.05f, 0.08f, "%.3f");
        ImGui::SliderFloat("Up",      &g_aimHeadUp,  -0.05f, 0.08f, "%.3f");
        ImGui::SliderFloat("Lift",    &g_aimHeadLift, -0.05f, 0.08f, "%.3f");
        ImGui::SameLine(); ImGui::TextDisabled("(no capsule fallback)");

        ImGui::Separator();
        ImGui::Text("%d in range", st.aimTargetCnt);
        ImGui::SameLine();
        if (st.aimSuppressed)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  released (shot)");
        else
            ImGui::TextColored(st.aimHeld ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                         : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                               st.aimHeld ? "  pulling" : "  idle");
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Trigger")) {
        ImGui::Checkbox("Enabled##trig", &g_trigEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled(g_aimEnabled ? "(fires at the aimed point)"
                                         : "(fires on the body)");
        const char* trigBtns[] = { "Right mouse held", "Always on" };
        ImGui::Combo("Activate", &g_trigButton, trigBtns, 2);

        ImGui::Separator();
        ImGui::TextDisabled("Hit test");
        const bool skelAimActive = g_aimEnabled && g_espSkeleton;
        if (skelAimActive) ImGui::BeginDisabled();
        ImGui::Checkbox("Use skeleton##trig", &g_trigSkeleton);
        ImGui::SameLine();
        ImGui::TextDisabled(skelAimActive ? "(uses the aimed bone)"
                            : g_trigSkeleton ? "(bones)" : "(box)");
        if (g_trigSkeleton) {
            const char* parts[] = { "Head", "Chest", "Body", "Legs", "All body" };
            ImGui::Combo("Shoot on", &g_trigSkelPart, parts, 5);
        }
        if (skelAimActive) ImGui::EndDisabled();

        ImGui::SliderFloat("Size scale", &g_trigSizeScale, 0.0f, 3.0f, "%.2f");
        ImGui::SameLine();
        ImGui::TextDisabled(g_trigSizeScale <= 0.01f ? "(pixels)" : "(scales w/ range)");
        ImGui::SliderFloat("Forgiveness (px)", &g_trigForgiveness, 0.0f, 25.0f, "%.1f");
        ImGui::SliderFloat("Min tolerance (px)", &g_trigMinTol, 0.0f, 8.0f, "%.1f");
        ImGui::SameLine(); ImGui::TextDisabled("(raise if far shots miss)");

        ImGui::Separator();
        ImGui::TextDisabled("Timing");
        ImGui::SliderInt("Arm delay (ms)", &g_trigActivateMs, 0, 500);
        ImGui::SliderInt("Delay (ms)",     &g_trigDelayMs,    0, 200);
        ImGui::SliderInt("Hold (ms)",      &g_trigHoldMs,     5, 120);
        ImGui::SliderInt("Cooldown (ms)",  &g_trigCooldownMs, 0, 400);

        ImGui::Separator();
        if (!g_trigEnabled)
            ImGui::TextDisabled("disabled");
        else if (!st.trigHeld)
            ImGui::TextDisabled("waiting for the activate button");
        else if (st.trigOnTarget)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "ON TARGET   off %.1f / tol %.1f px",
                               st.trigOnTargetPx, st.trigOnTargetTol);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "no target   off %.1f / tol %.1f px",
                               st.trigOnTargetPx, st.trigOnTargetTol);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Visibility")) {
        if (!st.visHave) {
            if (st.haveLastRenderTime)
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                                   "no render times; showing everyone");
            else
                ImGui::TextDisabled("Mesh_LastRenderTime is 0 in offsets.cfg");
            ImGui::Separator();
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("Aim: visible only",     &g_aimVisibleOnly);
        ImGui::Checkbox("Trigger: visible only", &g_trigVisibleOnly);
        ImGui::Spacing();
        ImGui::Combo("ESP style", &g_visStyle, "off\0X out hidden\0hide hidden\0");
        if (g_visStyle == 1) {
            ImGui::SliderInt("Hidden fade", &g_visDimAlpha, 0, 255);
            ImGui::SameLine(); ImGui::TextDisabled("(x opacity)");
            ImGui::SliderFloat("X width", &g_visXThick, 1.f, 8.f, "%.1f px");
        }
        ImGui::SliderFloat("Tolerance (s)", &g_visTolerance, 0.02f, 1.0f, "%.3f");
        ImGui::SameLine(); ImGui::TextDisabled("(lower drops cover faster)");
        if (!st.visHave) ImGui::EndDisabled();
        else {
            ImGui::Separator();
            ImGui::Text("%d visible", st.visVisibleCnt);
            ImGui::SameLine();
            ImGui::TextDisabled("/ %d hidden", st.visHiddenCnt);
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Sonar")) {
        ImGui::Checkbox("Enabled##sonar", &g_sonarEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(its own window, put it anywhere)");
        if (!g_sonarEnabled) ImGui::BeginDisabled();

        ImGui::SliderFloat("Range", &g_sonarRange, 10.f, 250.f, "%.0f m");
        ImGui::SameLine();
        ImGui::TextDisabled("(centre to rim)");
        ImGui::SliderInt("Opacity##sonar", &g_sonarAlpha, 30, 255);

        ImGui::Separator();
        ImGui::Checkbox("Class letters", &g_sonarLetters);
        ImGui::SameLine();
        ImGui::TextDisabled(g_sonarLetters ? "(L / M / H)" : "(plain dots)");
        ImGui::Checkbox("Range rings", &g_sonarRings);

        ImGui::Separator();
        ImGui::Checkbox("Lock in place", &g_sonarLock);
        ImGui::SameLine();
        ImGui::TextDisabled(g_sonarLock ? "(mouse ignored)"
                                        : "(drag to move, corner to resize)");
        ImGui::TextDisabled("%.0f,%.0f  %.0fx%.0f",
                            g_sonarX, g_sonarY, g_sonarW, g_sonarH);

        ImGui::Separator();
        ImGui::TextDisabled("It turns with you: up is where you are facing.");
        ImGui::TextDisabled("Squad colours match the ESP. Squadmates are not shown.");
        if (!g_sonarEnabled) ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Status")) {
        ImGui::Text("Entities  %d", st.entityCount);
        ImGui::SameLine();
        ImGui::TextDisabled("   %d FPS", st.fps);
        ImGui::Separator();
        ImGui::TextDisabled("Memory");
        if (st.usingKmod) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "kernel module   %llu reads",
                               (unsigned long long)st.kmodOk);
            if (st.kmodFellBack)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "fell back to process_vm_readv: %llu",
                                   (unsigned long long)st.kmodFellBack);
            else
                ImGui::TextDisabled("no fallbacks");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "process_vm_readv (module not loaded)");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Mouse");
        if (st.vmouseKernel)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "kernel injection, real pointer");
        else if (st.vmouseReady)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "uinput device (enumerable)");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "unavailable");
        ImGui::Separator();
        ImGui::SliderInt("Menu opacity", &g_menuAlpha, 40, 255);
        ImGui::Separator();
        ImGui::TextDisabled("INSERT menu   HOME aim   End quit");
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    }

    // Footer
    ImGui::Separator();
    ImGui::TextDisabled("by Ymsniper   GPLv2, no warranty");

    ImGui::End();
}


static volatile sig_atomic_t g_menuQuit = 0;
static void menuSignal(int) { g_menuQuit = 1; }

int menuMain(SharedState* sh) {
    // Follow the overlay out: without this a crash upstairs leaves the menu
    // behind as an orphan window.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    // Handlers inherited across fork() set a flag this loop never reads, so
    // SIGTERM would go unanswered and the overlay would wait forever on exit.
    // Take them over: leave the loop, publish, and go.
    signal(SIGINT,  menuSignal);
    signal(SIGTERM, menuSignal);

    SettingsMirror settings;
    settings.gen = sh->settingsGen.load();
    sharedReadFields(sh);
    mirrorCapture(settings);

    const int w = (int)(g_menuW > 200.f ? g_menuW : 380.f);
    const int h = (int)(g_menuH > 200.f ? g_menuH : 460.f);

    SetTraceLogLevel(LOG_WARNING);
    // An ordinary window: managed, resizable, focusable. Topmost so it is not
    // lost behind the game when it is called up. ALWAYS_RUN matters: while the
    // menu is hidden its window is unmapped, which GLFW reports as minimized,
    // and without it WindowShouldClose blocks in glfwWaitEvents -- the loop
    // would stall and never see the key that is supposed to bring it back.
    // HIDDEN so that starting the tool never activates anything: the window is
    // mapped for the first time when INSERT asks for it.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_TOPMOST |
                   FLAG_WINDOW_ALWAYS_RUN | FLAG_WINDOW_HIDDEN |
                   FLAG_WINDOW_TRANSPARENT);
    InitWindow(w, h, "Umbra");
    if (!IsWindowReady()) { printf("[menu] could not create the menu window\n"); return 1; }
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    rlImGuiSetup(true);
    // Same separate factors as the overlay: raylib's default alpha blend uses
    // the source alpha on the alpha channel too, which would make the panel
    // fade out faster than the opacity asked for.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                              RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);

    if (!ovl::init()) { printf("[menu] no X display; menu unavailable\n"); return 1; }
    ovl::adoptSelf("Umbra");

    // Never mapped yet, so nothing has been activated. It stays that way until
    // INSERT asks for it.
    bool visible = false;

    printf("[menu] ready - INSERT opens the settings window\n");
    fflush(stdout);

    bool prevInsert = false;
    while (!g_menuQuit) {
        if (getppid() == 1) break;             // the overlay is gone

        // The window's close button hides the menu rather than ending the
        // tool; End and Ctrl-C are how the tool is quit.
        if (WindowShouldClose() && visible) {
            visible = false;
            ovl::setVisible(false, false);
            ovl::focusGame(sh->gamePid);
        }

        const bool nowInsert = ovl::keyDown(ovl::KeyInsert);
        if (nowInsert && !prevInsert) {
            visible = !visible;
            ovl::setVisible(visible, true);
            sh->menuVisible.store(visible ? 1 : 0);
            printf("[menu] %s\n", visible ? "shown" : "hidden");
            fflush(stdout);
            // Hiding an ordinary window hands activation back the way closing
            // any window does, which is what makes the game re-grab the cursor.
            if (!visible) ovl::focusGame(sh->gamePid);
        }
        prevInsert = nowInsert;

        // Synced even while hidden, so a change made elsewhere is never missed.
        sharedSync(sh, settings);

        if (!visible) {
            // Nothing to draw, and no window to draw into. Idle cheaply.
            WaitTime(0.03);
            continue;
        }

        Status st = sh->status;

        BeginDrawing();
        ClearBackground(BLANK);
        BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            rlImGuiBegin();
            drawSettingsPanel(st);
            rlImGuiEnd();
        EndBlendMode();
        EndDrawing();

        g_menuW = (float)GetScreenWidth();
        g_menuH = (float)GetScreenHeight();
    }

    // Publish before leaving, so an edit made in the last frame is not lost.
    sharedSync(sh, settings);
    ovl::setVisible(false, false);
    ovl::focusGame(sh->gamePid);
    ovl::shutdown();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
