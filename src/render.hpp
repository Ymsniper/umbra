#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Everything drawn each frame: the ESP, the aim assist and
// triggerbot, and the ImGui settings menu.
#include <cstdint>
#include "vmouse.hpp"
#include <cstdlib>
#include <chrono>
#include "global.hpp"
#include "skeleton.hpp"
#include "structs.hpp"
#include <raylib.h>
#include <rlgl.h>
#include <imgui.h>
#include <rlImGui.h>
#include <cstdio>
#include <cmath>

// Colour helpers
// Builds a raylib Color from ints, clamped. Everything here computes channels
// from alphas and ratios, so going through one clamping helper keeps the
// narrowing conversions in a single place.
inline Color rgba(int r, int g, int b, int a = 255) {
    auto c8 = [](int v) -> unsigned char {
        return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return Color{c8(r), c8(g), c8(b), c8(a)};
}

inline Color squadColor(int squadIdx, bool isSelf) {
    if (isSelf) return rgba(0, 255, 120);
    static const Color palette[] = {
        rgba(255, 60,  60),
        rgba(60,  140, 255),
        rgba(255, 200, 0),
        rgba(200, 60,  255),
        rgba(0,   220, 220),
        rgba(255, 130, 0),
    };
    if (squadIdx < 0) return rgba(200, 200, 200);
    return palette[squadIdx % 6];
}

inline Color healthColor(double hp, double maxHp) {
    float ratio = (maxHp > 0) ? (float)(hp / maxHp) : 0.f;
    ratio = std::max(0.f, std::min(1.f, ratio));
    return rgba((int)(255 * (1.f - ratio)), (int)(255 * ratio), 0, 220);
}

// raylib has no text outline, and unoutlined labels disappear over bright
// scenery. Ring the glyphs in black first, then draw the text itself.
inline void drawTextOutlined(const Font& font, const char* s, float x, float y,
                             float size, Color col) {
    if (font.texture.id == 0 || !s || !*s) return;
    const Color edge = rgba(0, 0, 0, col.a);
    static const float ox[8] = {-1, 1,  0, 0, -1,  1, -1, 1};
    static const float oy[8] = { 0, 0, -1, 1, -1, -1,  1, 1};
    for (int i = 0; i < 8; i++)
        DrawTextEx(font, s, Vector2{x + ox[i], y + oy[i]}, size, 0.f, edge);
    DrawTextEx(font, s, Vector2{x, y}, size, 0.f, col);
}

inline void drawTextCentered(const Font& font, const char* s, float cx, float y,
                             float size, Color col) {
    if (font.texture.id == 0 || !s || !*s) return;
    const Vector2 m = MeasureTextEx(font, s, size, 0.f);
    drawTextOutlined(font, s, cx - m.x * 0.5f, y, size, col);
}

// Character capsule half-height in UE units. RootComponent sits at the capsule
// centre, so origin +/- this gives head and feet without needing bone data.
static constexpr float kCapsuleHalfHeight = 90.f;

// Draw a single entity
inline void drawEntity(const Font& font, const EntityData& ent,
                       const FMatrix& vp, int sw, int sh)
{
    // Style 2 hides what you cannot shoot; style 1 (default) keeps it on screen
    // but makes the shootable ones unmistakable. Both inert unless the offset
    // has been derived AND the data is currently trustworthy (g_visHave).
    const bool visKnown = (g_visHave && g_visStyle != 0);
    if (g_visStyle == 2 && g_visHave && !ent.visible) return;
    const bool visDim = (visKnown && g_visStyle == 1 && !ent.visible);
    static const bool eDbg = getenv("ESP_DEBUG") != nullptr;
    static int entDbg = 0;
    bool dbg = eDbg && ((entDbg++ % 240) == 0);
    if (!ent.valid) { if (dbg) printf("[draw] SKIP: ent.valid == false\n"); return; }
    if (ent.isSelf     && !g_espSelf)      return;
    if (ent.isTeammate && !g_espTeammates) return;

    Vec2 sRoot;
    if (!worldToScreen(vp, ent.origin, sRoot, sw, sh)) {
        if (dbg) printf("[draw] SKIP: origin not on screen\n");
        return;
    }
    if (dbg) printf("[draw] drawing ent at root=(%.0f,%.0f) dist=%.0fm\n",
                    sRoot.x, sRoot.y, ent.distance);

    const Color col     = squadColor(ent.squadIdx, ent.isSelf);
    const int   masterA = (g_espAlpha < 0) ? 0 : (g_espAlpha > 255 ? 255 : g_espAlpha);
    const float dimF    = ((g_visDimAlpha < 0) ? 0 :
                           (g_visDimAlpha > 255 ? 255 : g_visDimAlpha)) / 255.f;
    auto withA = [&](Color c, bool faded) {
        const float a = (float)masterA * (faded ? dimF : 1.f);
        return rgba(c.r, c.g, c.b, (int)a);
    };
    const Color colA  = withA(col, visDim);
    const float thick = std::max(0.5f, g_espThickness);

    // the composed rig
    if (g_espSkeleton && ent.hasRig && ent.rig && ent.rigCount > 1) {
        const skel::Rig* rg = static_cast<const skel::Rig*>(ent.rig);
        const Color rigCol = withA(rgba(120, 255, 160), visDim);
        static const int kBodyBones[] = { 1,  4,  7,  8,  9, 10, 11, 31, 32,
                                        33, 34, 65, 66, 67, 69, 70, 71 };
        auto isBodyBone = [](int b) {
            for (int x : kBodyBones) if (x == b) return true;
            return false;
        };
        for (int i = 1; i < ent.rigCount && i < rg->count; ++i) {
            int p = rg->parents[i];
            if (!isBodyBone(i)) continue;
            while (p > 0 && !isBodyBone(p)) p = rg->parents[p];
            // If the walk ran out without landing on a body bone it ends at 0,
            // the ROOT -- which sits on the ground between the feet. Drawing to
            // it produced a long line dangling from the pelvis to the floor.
            // No ancestor in the set means no link, not a link to the root.
            if (!isBodyBone(p)) continue;
            if (p < 0 || p >= ent.rigCount) continue;
            if (ent.rigBones[i].isZero() || ent.rigBones[p].isZero()) continue;
            Vec2 a, b;
            if (!worldToScreen(vp, ent.rigBones[i], a, sw, sh)) continue;
            if (!worldToScreen(vp, ent.rigBones[p], b, sw, sh)) continue;
            DrawLineEx(Vector2{a.x, a.y}, Vector2{b.x, b.y}, thick, rigCol);
        }
    }

    if (g_espBoxes) {
        Vec2 head{}, foot{};
        bool headOk = false, footOk = false;
        {
            // Prefer the real skeleton: the head bone is root-parented so it
            // needs no hierarchy, and ComponentToWorld's translation is the mesh
            // origin, which sits at the feet. Falls back to the capsule
            // approximation only when the skeleton could not be read.
            FVector hw, fw;
            if (ent.capsuleHalf > 0.f) {
                // Exact: the capsule is centred on the root, and the game sizes
                // it per class. No headroom fudge needed.
                hw = ent.origin; fw = ent.origin;
                hw.Z += ent.capsuleHalf;
                fw.Z -= ent.capsuleHalf;
            } else if (ent.hasSkeleton) {
                hw = ent.headWorld;
                fw = ent.feetWorld;
                // Scale the skull allowance with the character's own height so
                // Light / Medium / Heavy all cap correctly.
                double h = hw.Z - fw.Z;
                if (h > 1.0) hw.Z += h * (double)g_boxHeadroom;
            } else {
                hw = ent.origin; fw = ent.origin;
                hw.Z += kCapsuleHalfHeight;
                fw.Z -= kCapsuleHalfHeight;
            }
            Vec2 hs, fs;
            if (worldToScreen(vp, hw, hs, sw, sh) &&
                worldToScreen(vp, fw, fs, sw, sh)) {
                head = hs; foot = fs;
                headOk = footOk = true;
            }
        }

        static const bool bDbg = getenv("ESP_DEBUG") != nullptr;
        static int boxDbg = 0;
        if (bDbg && (boxDbg++ % 120) == 0)
            printf("[box] valid=%d espBoxes=%d headOk=%d footOk=%d head=(%.0f,%.0f) foot=(%.0f,%.0f)\n",
                   (int)ent.valid, (int)g_espBoxes, (int)headOk, (int)footOk,
                   head.x, head.y, foot.x, foot.y), fflush(stdout);

        if (headOk && footOk) {
            float h2f = foot.y - head.y;
            // The floor is off by default: clamping the height flattens the
            // depth cue, since every target past that range draws the same
            // size. Width is derived from the clamped height either way, so
            // the box keeps its proportions.
            if (g_boxMinPx > 0.f && h2f < g_boxMinPx) h2f = g_boxMinPx;
            const float width = h2f * 0.40f;   // human aspect over the full span
            const float cx    = (head.x + foot.x) * 0.5f;

            static const bool rDbg = getenv("ESP_DEBUG") != nullptr;
            static int rectDbg = 0;
            if (rDbg && (rectDbg++ % 120) == 0)
                printf("[rect] pos=(%.0f,%.0f) size=(%.0f,%.0f) col=(%d,%d,%d,%d)\n",
                       cx - width * 0.5f, head.y, width, h2f,
                       col.r, col.g, col.b, colA.a), fflush(stdout);

            DrawRectangleLinesEx(Rectangle{cx - width * 0.5f, head.y, width, h2f},
                                 thick, colA);

            // Behind cover -> cross it out. The box still shows WHERE he is and
            // WHICH squad he is on; the X says you cannot hit him from here.
            // Drawn at full master alpha rather than the dimmed one, so the
            // marker stays readable while the box itself fades back.
            if (visDim) {
                const float x0 = cx - width * 0.5f, x1 = cx + width * 0.5f;
                const float y0 = head.y,            y1 = head.y + h2f;
                const Color xc = withA(col, false);
                const float xt = std::max(1.f, g_visXThick);
                DrawLineEx(Vector2{x0, y0}, Vector2{x1, y1}, xt, xc);
                DrawLineEx(Vector2{x1, y0}, Vector2{x0, y1}, xt, xc);
            }

            // Snapline from the bottom centre of the screen. A thin distant box
            // is easy to miss; a line to it is not, and it makes misalignment
            // obvious immediately.
            if (g_espSnaplines)
                DrawLineEx(Vector2{(float)sw * 0.5f, (float)sh},
                           Vector2{cx, head.y + h2f},
                           std::max(1.f, thick * 0.6f), colA);

            // health bar on the left side
            if (g_espHealth && ent.maxHealth > 0) {
                const float barH = h2f;
                float ratio = (float)(ent.health / ent.maxHealth);
                ratio = std::max(0.f, std::min(1.f, ratio));
                const float bx = cx - width * 0.5f - 6.f;

                DrawRectangleRec(Rectangle{bx, head.y, 4.f, barH},
                                 rgba(40, 40, 40, (int)(180 * masterA / 255.f)));
                DrawRectangleRec(Rectangle{bx, head.y + barH * (1.f - ratio),
                                           4.f, barH * ratio},
                                 healthColor(ent.health, ent.maxHealth));
            }
        }
    }

    // text labels
    {
        char buf[128] = {};
        const float ts = std::max(6.f, g_espTextSize);
        float ty = sRoot.y - ts * 2.4f;

        if (g_espName && !ent.name.empty()) {
            drawTextCentered(font, ent.name.c_str(), sRoot.x, ty, ts,
                             withA(col, visDim));
            ty += ts + 2.f;
        }

        if (g_espHealth) {
            snprintf(buf, sizeof(buf), "%.0f/%.0f HP", ent.health, ent.maxHealth);
            drawTextCentered(font, buf, sRoot.x, ty, ts - 1.f,
                             withA(healthColor(ent.health, ent.maxHealth), visDim));
            ty += ts + 1.f;
        }

        if (g_espDistance) {
            snprintf(buf, sizeof(buf), "%.0fm", ent.distance);
            drawTextCentered(font, buf, sRoot.x, ty, ts - 2.f,
                             withA(rgba(200, 200, 200), visDim));
        }
    }
}

// Triggerbot helpers
inline bool crosshairOnBody(const EntityData& e, const FMatrix& vp,
                            int sw, int sh, float cx, float cy, float forgive) {
    if (e.capsuleHalf <= 20.f) return false;
    FVector top = e.origin, bot = e.origin;
    top.Z += e.capsuleHalf; bot.Z -= e.capsuleHalf;
    Vec2 ts, bs;
    if (!worldToScreen(vp, top, ts, sw, sh)) return false;
    if (!worldToScreen(vp, bot, bs, sw, sh)) return false;
    float h = bs.y - ts.y;
    if (h < 2.f) return false;
    float w  = h * 0.40f;                       // human aspect, like the ESP box
    float bx = (ts.x + bs.x) * 0.5f;
    return cx >= bx - w*0.5f - forgive && cx <= bx + w*0.5f + forgive
        && cy >= ts.y      - forgive && cy <= bs.y      + forgive;
}

inline float targetPixelHeight(const EntityData& e, const FMatrix& vp,
                               int sw, int sh) {
    if (e.capsuleHalf <= 20.f) return 0.f;
    FVector top = e.origin, bot = e.origin;
    top.Z += e.capsuleHalf; bot.Z -= e.capsuleHalf;
    Vec2 ts, bs;
    if (!worldToScreen(vp, top, ts, sw, sh)) return 0.f;
    if (!worldToScreen(vp, bot, bs, sw, sh)) return 0.f;
    const float h = bs.y - ts.y;
    return h > 0.f ? h : 0.f;
}

// The on-target tolerance in pixels for a given body part, scaled by how big the
// target actually is. `part`: 0 head, 1 chest, 2 body, 3 legs, 4 any bone.
// Fractions are of full body height: a head is ~12% tall, so ~6% radius.
inline float trigTolerance(const EntityData& e, const FMatrix& vp,
                           int sw, int sh, int part) {
    const float h = targetPixelHeight(e, vp, sw, sh);
    if (h <= 0.f) return g_trigForgiveness;      // no size -> flat pixels only
    const float frac = (part == 0) ? 0.060f      // head radius
                     : (part == 1) ? 0.110f      // chest
                     : (part == 2) ? 0.130f      // body/pelvis
                     : (part == 3) ? 0.070f      // legs
                                   : 0.045f;     // any bone / limb
    const float tol = g_trigForgiveness + h * frac * g_trigSizeScale;
    return tol < g_trigMinTol ? g_trigMinTol : tol;
}

// Standalone SKELETON test: is the crosshair on the chosen bone / any bone?
// part: 0 head, 1 chest, 2 body(pelvis), 3 legs(feet), 4 ALL body (any segment).
// `tol` is the already-size-scaled tolerance in pixels (see trigTolerance).
inline bool crosshairOnSkeleton(const EntityData& e, const FMatrix& vp,
                                int sw, int sh, float cx, float cy,
                                float tol, int part) {
    if (!e.rig || e.rigCount < 2) return false;
    const skel::Rig* rg = static_cast<const skel::Rig*>(e.rig);
    const float R = tol;
    const float R2 = R * R;

    auto proj = [&](int idx, Vec2& out) -> bool {
        if (idx < 0 || idx >= e.rigCount) return false;
        if (e.rigBones[idx].isZero()) return false;
        return worldToScreen(vp, e.rigBones[idx], out, sw, sh);
    };
    auto nearPoint = [&](int idx) -> bool {
        Vec2 p; if (!proj(idx, p)) return false;
        float dx = p.x - cx, dy = p.y - cy;
        return dx*dx + dy*dy <= R2;
    };

    if (part == 4) {
        // ALL BODY: crosshair near any drawn bone SEGMENT (covers long limbs).
        for (int i = 1; i < e.rigCount && i < rg->count; ++i) {
            int pa = (i < (int)rg->parents.size()) ? rg->parents[i] : -1;
            Vec2 a, b;
            if (!proj(i, a) || !proj(pa, b)) continue;
            const float vx = b.x-a.x, vy = b.y-a.y;
            const float wx = cx-a.x, wy = cy-a.y;
            const float len2 = vx*vx + vy*vy;
            float t = len2 > 0.f ? (wx*vx + wy*vy) / len2 : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const float px = a.x + t*vx - cx, py = a.y + t*vy - cy;
            if (px*px + py*py <= R2) return true;
        }
        return false;
    }
    switch (part) {
        case 0: return nearPoint(rg->head);
        case 1: return nearPoint(rg->chest);
        case 2: return nearPoint(rg->pelvis);
        default: return nearPoint(rg->lFoot) || nearPoint(rg->rFoot);  // legs
    }
}

// Click state machine, called every frame with wantFire = "on target now".
// Handles delay-before-shot, click hold duration, and a cooldown between shots,
// and always releases a held click when the target is lost or trigger released.
inline void triggerUpdate(bool active, bool wantFire) {
    using clock = std::chrono::steady_clock;
    static auto lastShot    = clock::now() - std::chrono::hours(1);
    static auto heldSince   = clock::time_point{};
    static auto streakStart = clock::time_point{};   // start of on-target streak
    static auto lastOnTgt   = clock::time_point{};    // last frame seen on-target
    static auto clickStart  = clock::time_point{};
    static bool clicking    = false;
    static int  onFrames    = 0;
    const auto now = clock::now();
    auto ms = [](auto d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };

    // release a held click once the hold duration is up
    if (clicking && ms(now - clickStart) >= g_trigHoldMs) {
        g_vmouse.press(false); clicking = false; lastShot = now;
    }

    // ARM DELAY: time since the button was first held. Until it elapses the
    // triggerbot is not armed, so ADS'ing does not insta-fire.
    if (active) { if (heldSince == clock::time_point{}) heldSince = now; }
    else        { heldSince = clock::time_point{}; if (clicking) { g_vmouse.press(false); clicking = false; } }
    const bool armed = active && ms(now - heldSince) >= g_trigActivateMs;

    // ON-TARGET tracking with a GRACE window. A single-frame projection dropout
    // (the "sometimes doesn't shoot" case) must not reset the streak, so a brief
    // flicker to off-target within GRACE_MS keeps it alive.
    const long GRACE_MS = 45;
    if (active && wantFire) {
        if (streakStart == clock::time_point{} || ms(now - lastOnTgt) > GRACE_MS) {
            streakStart = now; onFrames = 0;         // start a fresh streak
        }
        lastOnTgt = now; onFrames++;
    } else if (streakStart != clock::time_point{} && ms(now - lastOnTgt) > GRACE_MS) {
        streakStart = clock::time_point{}; onFrames = 0;   // truly off target
    }

    // Fire needs: armed; a 2-FRAME confirmation (kills single-frame false fires,
    // the "sometimes shoots when it shouldn't" case, and is framerate-independent);
    // the user's on-target delay; the cooldown; and on-target THIS frame.
    const bool ready = streakStart != clock::time_point{} && onFrames >= 2
                       && ms(now - streakStart) >= g_trigDelayMs;
    if (!clicking && armed && ready && wantFire
        && ms(now - lastShot) >= g_trigCooldownMs) {
        g_vmouse.press(true); clicking = true; clickStart = now;
    }
}

// ImGui settings panel
inline void drawSettingsPanel() {
    ImGui::SetNextWindowSize(ImVec2(g_menuW, g_menuH), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
    ImGui::Begin("Umbra", nullptr, ImGuiWindowFlags_NoCollapse);

    // Header: name, and the two facts worth seeing without opening a tab.
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "UMBRA");
    ImGui::SameLine();
    ImGui::TextDisabled("| %d entities", g_entityCount);
    if (g_aimEnabled) {
        ImGui::SameLine();
        ImGui::TextColored(g_aimSuppressed ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                           : g_aimHeld       ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           g_aimSuppressed ? "| aim released"
                           : g_aimHeld     ? "| AIM" : "| aim");
    }
    if (g_trigEnabled) {
        ImGui::SameLine();
        ImGui::TextColored(g_trigOnTarget ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                          : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           g_trigOnTarget ? "| TRIG" : "| trig");
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
                            g_camView.FOV, g_camView.FOV * g_fovScale);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Aim")) {
        ImGui::Checkbox("Enabled##aim", &g_aimEnabled);
        ImGui::SameLine();
        if (g_vmouse.ready())
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
        // Only meaningful when the aim is on a different button from the shot.
        // On "Left mouse" or "Either" the trigger IS the aim button, so firing
        // would release and re-arm in the same instant and do nothing at all.
        const bool qsUsable = (g_aimButton == 0);
        if (!qsUsable) ImGui::BeginDisabled();
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
        ImGui::Text("%d in range", g_aimTargetCnt);
        ImGui::SameLine();
        if (g_aimSuppressed)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  released (shot)");
        else
            ImGui::TextColored(g_aimHeld ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                         : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                               g_aimHeld ? "  pulling" : "  idle");
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
        else if (!g_trigHeld)
            ImGui::TextDisabled("waiting for the activate button");
        else if (g_trigOnTarget)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "ON TARGET   off %.1f / tol %.1f px",
                               g_trigOnTargetPx, g_trigOnTargetTol);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "no target   off %.1f / tol %.1f px",
                               g_trigOnTargetPx, g_trigOnTargetTol);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Visibility")) {
        if (!g_visHave) {
            if (g_off.Mesh_LastRenderTime)
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
        if (!g_visHave) ImGui::EndDisabled();
        else {
            ImGui::Separator();
            ImGui::Text("%d visible", g_visVisibleCnt);
            ImGui::SameLine();
            ImGui::TextDisabled("/ %d hidden", g_visHiddenCnt);
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Status")) {
        ImGui::Text("Entities  %d", g_entityCount);
        ImGui::SameLine();
        ImGui::TextDisabled("   %d FPS", GetFPS());
        ImGui::Separator();
        ImGui::TextDisabled("Memory");
        if (g_mem.usingKmod()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "kernel module   %llu reads",
                               (unsigned long long)g_mem.kmodOk);
            if (g_mem.kmodFellBack)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "fell back to process_vm_readv: %llu",
                                   (unsigned long long)g_mem.kmodFellBack);
            else
                ImGui::TextDisabled("no fallbacks");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "process_vm_readv (module not loaded)");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Mouse");
        if (g_vmouse.usingKernel())
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "kernel injection, real pointer");
        else if (g_vmouse.ready())
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "uinput device (enumerable)");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "unavailable");
        ImGui::Separator();
        ImGui::TextDisabled("INSERT menu   HOME aim   End quit");
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    }

    { ImVec2 sz = ImGui::GetWindowSize(); g_menuW = sz.x; g_menuH = sz.y; }

    // Footer
    ImGui::Separator();
    ImGui::TextDisabled("by Ymsniper   GPLv2, no warranty");

    ImGui::End();
}

// Main render frame
inline void renderFrame(const Font& font)
{
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();

    ViewInfo vi;
    {
        std::lock_guard<std::mutex> lk(g_camMtx);
        vi = g_camView;
    }
    FMatrix vp = buildVPMatrix(vi, sw, sh);

    {
        std::lock_guard<std::mutex> lk(g_entityMtx);
        // Entity count is correct and live, yet nothing appears on screen, so
        // the break is between world coords and pixels. Print the camera and
        // the first entity's projection so the failing stage is visible.
        static const bool espDbg = getenv("ESP_DEBUG") != nullptr;
        static int dbg = 0;
        if (espDbg && (dbg++ % 120) == 0) {
            printf("[proj] cam loc %.0f %.0f %.0f  rot %.1f %.1f %.1f  fov %.1f  screen %dx%d  ents %d\n",
                   vi.Location.X, vi.Location.Y, vi.Location.Z,
                   vi.Rotation.Pitch, vi.Rotation.Yaw, vi.Rotation.Roll,
                   vi.FOV, sw, sh, g_entityCount);
            for (int i = 0; i < g_entityCount && i < 3; i++) {
                Vec2 sp;
                bool ok = worldToScreen(vp, g_entities[i].origin, sp, sw, sh);
                printf("       ent[%d] world %.0f %.0f %.0f -> %s (%.0f, %.0f)  dist %.0fm\n",
                       i, g_entities[i].origin.X, g_entities[i].origin.Y,
                       g_entities[i].origin.Z, ok ? "ON " : "OFF", sp.x, sp.y,
                       g_entities[i].distance);
            }
            fflush(stdout);
        }
        for (int i = 0; i < g_entityCount; i++)
            drawEntity(font, g_entities[i], vp, sw, sh);
    }

    // aim assist
    const bool aimActive  = g_aimEnabled  && g_aimHeld && !g_aimSuppressed;
    const bool trigActive = g_trigEnabled && g_trigHeld;
    bool trigWantFire = false;

    // Targets inside this ring are the ones the aim will take.
    if (g_aimShowFov && g_aimEnabled)
        DrawCircleLinesV(Vector2{sw * 0.5f, sh * 0.5f}, g_aimFovPx,
                         rgba(255, 255, 255, g_espAlpha / 2));

    if ((aimActive || trigActive) && g_vmouse.ready()) {
        std::lock_guard<std::mutex> lk(g_entityMtx);
        const float cx = sw * 0.5f, cy = sh * 0.5f;
        // Behind cover -> not a candidate. Gated on g_visHave so this is a
        // NO-OP until Mesh_LastRenderTime is derived (every player reads
        // visible while the offset is 0), and on the mode that is actually
        // running, so the aim and trigger each honour their own switch.
        const bool visFilter = g_visHave &&
            ((aimActive && g_aimVisibleOnly) || (trigActive && g_trigVisibleOnly));
        float bestScore = 1e30f;
        float bestX = 0.f, bestY = 0.f;
        bool  found = false;
        int   bestIdx = -1;
        // The TRIGGER must test whoever is actually UNDER the crosshair -- not
        // the aimbot's biased pick, and not gated by the aim FOV. Track the
        // nearest-to-crosshair target independently.
        float bestPix    = 1e30f;
        int   bestPixIdx = -1;
        float stickyScore = 1e30f;          // the held target's score, if alive
        int   considered = 0, skelCnt = 0, noSkelCnt = 0;
        bool  lockSkel = false;
        Vec2  lockPt{};
        float lockDist = 0.f;

        for (int i = 0; i < g_entityCount; i++) {
            const EntityData& e = g_entities[i];
            if (!e.valid || e.isSelf || e.isTeammate) continue;   // never squadmates
            if (visFilter && !e.visible) continue;                  // behind cover
            if (e.distance > g_aimMaxDist) continue;

            // The aim point comes from the SKELETON when there is one. The
            // head bone is measured, not guessed -- readBones finds it
            // geometrically every frame -- so a headshot aims at that player's
            // actual head rather than at a fraction of a capsule.
            FVector aim;
            bool    usedSkel = false;
            if (e.hasSkeleton) {
                usedSkel = true;
                const double bodyH = e.headWorld.Z - e.feetWorld.Z;
                if (g_aimBone == 0 && e.hasHeadJoint) {
                    aim = e.headJoint;
                } else if (g_aimBone == 0 && e.eyeHeight > 0.f) {
                    // no rig this frame: the game's own eye height, exact per
                    // pawn and correct per class, on the body axis
                    aim = e.origin;
                    aim.Z += (double)e.eyeHeight;
                } else if (e.capsuleHalf > 20.f) {
                    const double half = (double)e.capsuleHalf;
                    const double feetZ = e.origin.Z - half, fullH = half * 2.0;
                    const double t = (g_aimBone == 0) ? (double)kHeadCapsuleT
                                   : (g_aimBone == 1) ? 0.78
                                   : (g_aimBone == 2) ? 0.55 : 0.25;
                    aim = e.origin;
                    aim.Z = feetZ + fullH * t;
                } else if (g_aimBone == 0) {
                    aim = e.headWorld;
                    aim.Z += bodyH * (double)g_aimHeadLift;
                } else {
                    double t = (g_aimBone == 1) ? 0.78     // chest
                             : (g_aimBone == 2) ? 0.55     // body
                                                : 0.25;    // legs
                    aim = FVector(e.feetWorld.X + (e.headWorld.X - e.feetWorld.X) * t,
                                  e.feetWorld.Y + (e.headWorld.Y - e.feetWorld.Y) * t,
                                  e.feetWorld.Z + (e.headWorld.Z - e.feetWorld.Z) * t);
                }
            } else {
                double half = (e.capsuleHalf > 20.f) ? (double)e.capsuleHalf : 90.0;
                aim = e.origin;
                aim.Z += (g_aimBone == 0) ? half * 0.95
                       : (g_aimBone == 1) ? half * 0.45
                       : (g_aimBone == 2) ? 0.0
                                          : -half * 0.60;
            }

            // LEAD. Cancels the constant trail described at g_aimPredict: the
            // aim point is where the target was, so push it forward by the
            // measured velocity over the latency. Applies to the trigger too,
            // since the trigger tests this same projected point.
            if (g_aimPredict && g_aimLeadMs > 0.f) {
                const double lead = (double)g_aimLeadMs * 0.001;
                aim.X += e.velocity.X * lead;
                aim.Y += e.velocity.Y * lead;
                aim.Z += e.velocity.Z * lead;
            }

            Vec2 sp;
            if (!worldToScreen(vp, aim, sp, sw, sh)) continue;    // behind camera
            const float dx = sp.x - cx, dy = sp.y - cy;
            const float pix = std::sqrt(dx * dx + dy * dy);

            // trigger's crosshair-nearest target (before any FOV gate)
            if (pix < bestPix) { bestPix = pix; bestPixIdx = i; }

            float fovLimit = g_aimFovPx;
            if (g_aimDistFov) {
                const float d = e.distance;
                fovLimit *= (d > 50.f) ? 1.0f : (5.0f - d / 12.5f);
            }
            if (pix > fovLimit) continue;
            considered++;
            if (usedSkel) skelCnt++; else noSkelCnt++;

            const float nPix  = pix / (fovLimit > 1.f ? fovLimit : 1.f);
            const float nDist = (g_aimMaxDist > 1.f)
                              ? (e.distance / g_aimMaxDist) : 0.f;
            const float bias  = (g_aimTargetBias < 0.f) ? 0.f
                              : (g_aimTargetBias > 1.f) ? 1.f : g_aimTargetBias;
            const float score = bias * nPix + (1.0f - bias) * nDist;

            if (g_aimSticky && i == g_aimLockedIdx) stickyScore = score;

            if (score < bestScore) {
                bestScore = score; bestX = dx; bestY = dy; found = true;
                bestIdx = i;
                lockSkel = usedSkel; lockPt = sp; lockDist = e.distance;
            }
        }

        if (g_aimSticky && g_aimLockedIdx >= 0 && stickyScore < 1e29f
            && bestIdx != g_aimLockedIdx
            && bestScore * g_aimStickiness > stickyScore) {
            const EntityData& held = g_entities[g_aimLockedIdx];
            Vec2 hp;
            FVector ha = held.hasHeadJoint ? held.headJoint : held.origin;
            if (worldToScreen(vp, ha, hp, sw, sh)) {
                bestX = hp.x - cx; bestY = hp.y - cy;
                bestIdx = g_aimLockedIdx; lockPt = hp; lockDist = held.distance;
            }
        }
        g_aimLockedIdx = found ? bestIdx : -1;
        g_aimTargetCnt = considered;
        g_aimSkelCnt   = skelCnt;
        g_aimNoSkelCnt = noSkelCnt;
        g_aimLockSkel  = found && lockSkel;
        g_aimLockPx    = found ? std::sqrt(bestX*bestX + bestY*bestY) : 0.f;
        g_aimLockPt    = lockPt;
        g_aimLockDist  = found ? lockDist : 0.f;

        if (found && aimActive) {
            float pullX = bestX, pullY = bestY;

            // CURVED PULL, applied before smoothing so it shapes the path while
            // smoothing still governs the rate. Rotating part of the delta into
            // its perpendicular makes the approach arc rather than run straight
            // at the target; a dead-straight line is the shape no hand draws.
            if (g_aimCurve) {
                auto jit = [](float v, float pct) {
                    if (pct <= 0.f) return v;
                    const float r = ((float)rand() / (float)RAND_MAX) * 2.f - 1.f;
                    return v * (1.f + r * pct * 0.01f);
                };
                const float ccx = std::max(0.10f, jit(g_aimCurveX, g_aimCurveJitter));
                const float ccy = std::max(0.10f, jit(g_aimCurveY, g_aimCurveJitter));
                // Sign follows the direction of travel, so the arc bends the same
                // way whichever side the target is on, and flips with "above".
                const float sgn = (g_aimCurveAbove ? -1.f : 1.f)
                                * ((bestX >= 0.f) ? 1.f : -1.f);
                pullY = bestY + sgn * (bestX / ccx);
                pullX = bestX + sgn * (bestY / ccy);
            }

            const float s = g_aimSmooth < 1.f ? 1.f : g_aimSmooth;
            const float mx = pullX / s, my = pullY / s;
            if (g_aimSmoothMode == 1) {
                const float in = (g_aimInertia < 0.f) ? 0.f
                               : (g_aimInertia > 1.f) ? 1.f : g_aimInertia;
                const float alpha = 1.0f - in * 0.5f;
                g_aimInertiaAccX += (mx - g_aimInertiaAccX) * alpha;
                g_aimInertiaAccY += (my - g_aimInertiaAccY) * alpha;
                g_vmouse.moveRel(g_aimInertiaAccX, g_aimInertiaAccY);
            } else {
                g_vmouse.moveRel(mx, my);
            }
        }

        // triggerbot decision
        if (trigActive && aimActive && found && g_aimLockedIdx >= 0) {
            // COMBINED: the aimbot is pulling toward g_aimLockedIdx. Fire only
            // when the crosshair has actually reached that aim point, within the
            // angular size of the part being aimed at.
            const EntityData& t = g_entities[g_aimLockedIdx];
            const float tol = trigTolerance(t, vp, sw, sh, g_aimBone);
            g_trigOnTargetTol = tol;
            g_trigOnTargetPx  = g_aimLockPx;
            trigWantFire = (g_aimLockPx <= tol);
        } else if (trigActive && bestPixIdx >= 0
                   && !(g_trigVisibleOnly && g_visHave
                        && !g_entities[bestPixIdx].visible)) {
            const EntityData& t = g_entities[bestPixIdx];
            g_trigOnTargetPx = bestPix;
            if (g_trigSkeleton && t.rig && t.rigCount > 1) {
                const float tol = trigTolerance(t, vp, sw, sh, g_trigSkelPart);
                g_trigOnTargetTol = tol;
                trigWantFire = crosshairOnSkeleton(t, vp, sw, sh, cx, cy,
                                                   tol, g_trigSkelPart);
            } else {
                // box: the box itself already scales with distance; the margin
                // added around it must scale too or it dominates at range.
                const float tol = trigTolerance(t, vp, sw, sh, 4);
                g_trigOnTargetTol = tol;
                trigWantFire = crosshairOnBody(t, vp, sw, sh, cx, cy, tol);
            }
        } else {
            g_trigOnTargetTol = 0.f; g_trigOnTargetPx = 0.f;
        }
        g_trigOnTarget = trigWantFire;      // for the menu diagnostic
    }

    else { g_aimInertiaAccX = g_aimInertiaAccY = 0.f; g_aimLockedIdx = -1; }

    // fire (or release) the shot; runs every frame so a held click always ends
    triggerUpdate(trigActive, trigWantFire);

    // rlImGuiBegin must run every frame even when the panel is hidden: it
    // starts ImGui's frame and feeds it input. Only the panel itself is skipped.
    rlImGuiBegin();
    if (g_menuVisible.load()) drawSettingsPanel();
    rlImGuiEnd();
}
