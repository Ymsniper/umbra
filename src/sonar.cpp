// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// A live top-down view of the enemies around you, in a window of its own so it
// can sit anywhere, including on a second screen. It runs as a separate process
// for the same reason the menu does; see shared.hpp.
#include "shared.hpp"
#include "x11_overlay.hpp"
#include "colors.hpp"
#include "font.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <cmath>
#include <cstdio>
#include <sys/prctl.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t g_sonarQuit = 0;
static void sonarSignal(int) { g_sonarQuit = 1; }

namespace {

constexpr float kUnitsPerMetre = 100.f;   // UE works in centimetres
constexpr int   kGripPx        = 16;      // resize corner, in pixels
constexpr int   kMinSide       = 120;

const char* classLetter(uint8_t cls) {
    switch (cls) {
        case 0:  return "L";
        case 1:  return "M";
        case 2:  return "H";
        default: return "?";
    }
}

// The dial: a disc, the range rings, and a marker for you at the centre facing
// up. Everything is scaled to the smaller side so the dial stays round when the
// window is not.
void drawDial(const Font& font, float cx, float cy, float radius, int alpha) {
    DrawCircleV(Vector2{cx, cy}, radius, rgba(10, 12, 16, alpha * 70 / 255));
    DrawCircleLinesV(Vector2{cx, cy}, radius, rgba(150, 160, 175, alpha));

    if (g_sonarRings) {
        const Color faint = rgba(150, 160, 175, alpha * 45 / 255);
        DrawCircleLinesV(Vector2{cx, cy}, radius * 0.66f, faint);
        DrawCircleLinesV(Vector2{cx, cy}, radius * 0.33f, faint);
        DrawLineEx(Vector2{cx - radius, cy}, Vector2{cx + radius, cy}, 1.f, faint);
        DrawLineEx(Vector2{cx, cy - radius}, Vector2{cx, cy + radius}, 1.f, faint);
    }

    // You, facing up. The dial rotates with you rather than the marker turning.
    const float s = radius * 0.058f;
    const Color me = neon(rgba(0, 255, 120), 0.15f);
    DrawTriangle(Vector2{cx, cy - s * 1.6f},
                 Vector2{cx - s, cy + s},
                 Vector2{cx + s, cy + s}, rgba(me.r, me.g, me.b, alpha));
}

void drawBlip(const Font& font, const SonarBlip& b, float cx, float cy,
              float radius, float unitsToPx, int alpha, float yawRad) {
    // Into view space: forward is (cos yaw, sin yaw), right is (-sin yaw,
    // cos yaw). Forward goes up the window, so it subtracts from y.
    const float c = cosf(yawRad), s = sinf(yawRad);
    const float fwd   =  b.dx * c + b.dy * s;
    const float right = -b.dx * s + b.dy * c;

    const float px = cx + right * unitsToPx;
    const float py = cy - fwd   * unitsToPx;
    const float ox = px - cx, oy = py - cy;
    if (ox * ox + oy * oy > radius * radius) return;    // outside the range

    const Color col = neon(squadColor(b.squad, false));
    const Color solid = rgba(col.r, col.g, col.b, alpha);

    if (!g_sonarLetters || font.texture.id == 0) {
        DrawCircleV(Vector2{px, py}, 3.5f, solid);
        DrawCircleLinesV(Vector2{px, py}, 3.5f, rgba(0, 0, 0, alpha));
        return;
    }

    const char*   letter = classLetter(b.cls);
    const float   size   = radius * 0.165f < 12.f ? 12.f : radius * 0.165f;
    const Vector2 m      = MeasureTextEx(font, letter, size, 0.f);
    const float   lx     = px - m.x * 0.5f;
    const float   ly     = py - m.y * 0.5f;

    // Ringed in black so a letter stays legible against whatever is behind the
    // window when the sonar is see-through.
    const Color edge = rgba(0, 0, 0, alpha);
    static const float dx[8] = {-1, 1,  0, 0, -1,  1, -1, 1};
    static const float dy[8] = { 0, 0, -1, 1, -1, -1,  1, 1};
    for (int i = 0; i < 8; i++)
        DrawTextEx(font, letter, Vector2{lx + dx[i], ly + dy[i]}, size, 0.f, edge);
    DrawTextEx(font, letter, Vector2{lx, ly}, size, 0.f, solid);
}

// Click and drag anywhere to move it, or the bottom-right corner to resize.
// The pointer is queried rather than taken from events, so this works whether
// or not the window happens to hold focus.
void handleMouse(bool& dragging, bool& resizing, int& grabX, int& grabY) {
    static bool prevDown = false;

    int mx = 0, my = 0; bool lmb = false, rmb = false;
    if (!ovl::pointer(mx, my, lmb, rmb)) { prevDown = false; return; }

    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (!ovl::selfRect(wx, wy, ww, wh)) { prevDown = false; return; }

    const bool inside = (mx >= 0 && my >= 0 && mx < ww && my < wh);

    if (lmb && !prevDown && inside && !g_sonarLock) {
        resizing = (mx >= ww - kGripPx && my >= wh - kGripPx);
        dragging = !resizing;
        grabX = mx;
        grabY = my;
    } else if (!lmb) {
        if (dragging || resizing) {
            g_sonarX = (float)wx; g_sonarY = (float)wy;
            g_sonarW = (float)ww; g_sonarH = (float)wh;
        }
        dragging = resizing = false;
    }

    if (resizing) {
        const int nw = mx < kMinSide ? kMinSide : mx;
        const int nh = my < kMinSide ? kMinSide : my;
        if (nw != ww || nh != wh) ovl::moveResizeManaged(wx, wy, nw, nh);
    } else if (dragging) {
        const int nx = wx + (mx - grabX);
        const int ny = wy + (my - grabY);
        if (nx != wx || ny != wy) ovl::moveResizeManaged(nx, ny, ww, wh);
    }
    prevDown = lmb;
}

}  // namespace

int sonarMain(SharedState* sh) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGINT,  sonarSignal);
    signal(SIGTERM, sonarSignal);

    SettingsMirror settings;
    settings.gen = sh->settingsGen.load();
    sharedReadFields(sh);
    mirrorCapture(settings);

    const int w0 = (int)(g_sonarW > kMinSide ? g_sonarW : 260.f);
    const int h0 = (int)(g_sonarH > kMinSide ? g_sonarH : 260.f);

    SetTraceLogLevel(LOG_WARNING);
    // Borderless and see-through, above ordinary windows, and created hidden so
    // that starting the tool activates nothing.
    SetConfigFlags(FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED |
                   FLAG_WINDOW_RESIZABLE   | FLAG_WINDOW_TOPMOST |
                   FLAG_WINDOW_ALWAYS_RUN  | FLAG_WINDOW_HIDDEN);
    InitWindow(w0, h0, "Umbra Sonar");
    if (!IsWindowReady()) { printf("[sonar] could not create the window\n"); return 1; }
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                              RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    Font font = loadUiFont();

    if (!ovl::init()) { printf("[sonar] no X display\n"); return 1; }
    ovl::adoptSelf("Umbra Sonar");
    ovl::setNoFocusSteal();
    ovl::setAlwaysOnTop();

    bool shown = false, dragging = false, resizing = false;
    int  grabX = 0, grabY = 0;
    printf("[sonar] ready\n");
    fflush(stdout);

    while (!g_sonarQuit) {
        if (getppid() == 1) break;              // the overlay is gone
        sharedSync(sh, settings);

        if (!g_sonarEnabled) {
            if (shown) {
                ovl::setVisible(false, false);
                shown = false;
                printf("[sonar] off\n"); fflush(stdout);
            }
            WaitTime(0.05);
            continue;
        }
        if (!shown) {
            const int px = (int)g_sonarX, py = (int)g_sonarY;
            const int pw = (int)g_sonarW, ph = (int)g_sonarH;
            ovl::setSizeHints(px, py, pw, ph);
            // Shown without activation: the game keeps the cursor.
            ovl::setVisible(true, false);
            ovl::moveResizeManaged(px, py, pw, ph);
            shown = true;
            printf("[sonar] on at %d,%d %dx%d\n", px, py, pw, ph); fflush(stdout);
        }

        handleMouse(dragging, resizing, grabX, grabY);

        SonarFrame frame = sh->sonar;
        const int   sw     = GetScreenWidth();
        const int   sh_px  = GetScreenHeight();
        const float cx     = sw * 0.5f, cy = sh_px * 0.5f;
        const float radius = (sw < sh_px ? sw : sh_px) * 0.5f - 6.f;
        const float range  = g_sonarRange < 5.f ? 5.f : g_sonarRange;
        const float toPx   = radius / (range * kUnitsPerMetre);
        const int   alpha  = g_sonarAlpha < 0 ? 0 : (g_sonarAlpha > 255 ? 255 : g_sonarAlpha);
        const float yawRad = frame.yaw * (float)(M_PI / 180.0);

        BeginDrawing();
        ClearBackground(BLANK);
        BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            drawDial(font, cx, cy, radius, alpha);
            for (int i = 0; i < frame.count && i < kSharedBlips; i++)
                drawBlip(font, frame.blips[i], cx, cy, radius, toPx, alpha, yawRad);
            if (!g_sonarLock) {
                // A corner to grab, only as a hint that it is draggable.
                const Color grip = rgba(150, 160, 175, alpha * 60 / 255);
                DrawLineEx(Vector2{(float)sw - 3.f, (float)sh_px - kGripPx},
                           Vector2{(float)sw - 3.f, (float)sh_px - 3.f}, 2.f, grip);
                DrawLineEx(Vector2{(float)sw - kGripPx, (float)sh_px - 3.f},
                           Vector2{(float)sw - 3.f,     (float)sh_px - 3.f}, 2.f, grip);
            }
        EndBlendMode();
        EndDrawing();

    }

    sharedSync(sh, settings);
    ovl::setVisible(false, false);
    ovl::shutdown();
    if (font.texture.id != GetFontDefault().texture.id) UnloadFont(font);
    CloseWindow();
    return 0;
}
