#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Off-screen indicators: a pin along the window's edge, or on a ring around the
// crosshair, for every player the ESP cannot draw because they are behind you,
// beside you, or past an edge of the screen.
//
// In front of you a pin points exactly where the player projects onto your view,
// through the same projection the ESP's boxes and lines use, so the pin and the
// line to the same player always agree. Behind you that projection flips and
// means nothing, and the ESP draws no line there either, so the pin falls back
// to the player's bearing, radar-style: straight behind is straight down, and
// it cannot spin when they are right behind you. Height, which the bearing
// cannot show, gets a marker beside the distance.
#include "colors.hpp"
#include "global.hpp"
#include "structs.hpp"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace oof {

inline constexpr Color kNeon = {57, 255, 20, 255};   // #39FF14

// What has to outlive a frame for one player: how far the pin has faded in, and
// where its flicker is. The flicker's phase is advanced by its current rate
// rather than computed from the clock, because rate x time jumps every time the
// rate changes, and the rate changes every frame someone walks closer.
struct Track {
    float  fade  = 0.f;
    double phase = 0.0;
    double seen  = 0.0;
};

inline std::unordered_map<uintptr_t, Track>& tracks() {
    static std::unordered_map<uintptr_t, Track> t;
    return t;
}

// 0 Light, 1 Medium, 2 Heavy, -1 not known. Only a max health near one of the
// three class totals counts: with the health component unreadable the value is
// a placeholder, and naming a class from it would name the wrong one.
inline int classOf(double maxHp) {
    static const double totals[3] = {150.0, 250.0, 350.0};
    for (int i = 0; i < 3; i++)
        if (std::fabs(maxHp - totals[i]) <= 40.0) return i;
    return -1;
}

inline Color withAlpha(Color c, float a) {
    return rgba(c.r, c.g, c.b, (int)std::lround(std::clamp(a, 0.f, 255.f)));
}

// Dark ink on light fills and light ink on dark ones, by perceived brightness.
inline bool wantsDarkInk(Color c) {
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.f > 0.55f;
}

// raylib culls triangles wound the wrong way, and expects them counter-clockwise
// as they appear on screen, which with y pointing down is a negative cross.
inline void fillTri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0.f) std::swap(b, c);
    DrawTriangle(a, b, c, col);
}

// The pin: a disc with a tip pointing along `u`, as one fan from the centre so a
// translucent fill has no seam where a separate circle and triangle overlap.
// `tip` is the distance from the centre to the point.
inline void fillPin(Vector2 C, Vector2 u, float r, float tip, Color col) {
    const float base = std::atan2(u.y, u.x);
    const float half = std::acos(std::clamp(r / tip, -1.f, 1.f));   // tangent points
    constexpr int kArc = 28;
    Vector2 pts[kArc + 2];
    int n = 0;
    pts[n++] = Vector2{C.x + u.x * tip, C.y + u.y * tip};
    const float a0 = base + half, a1 = base + 2.f * PI - half;
    for (int i = 0; i <= kArc; i++) {
        const float a = a0 + (a1 - a0) * (float)i / (float)kArc;
        pts[n++] = Vector2{C.x + r * std::cos(a), C.y + r * std::sin(a)};
    }
    for (int i = 0; i < n; i++) fillTri(C, pts[i], pts[(i + 1) % n], col);
}

// Glyph outlined in a contrasting colour, as the ESP labels are, so it holds up
// over bright scenery and dark alike.
inline void label(const Font& font, const char* s, float x, float y, float size,
                  Color ink, Color edge, float thick = 1.f) {
    if (font.texture.id == 0 || !s || !*s) return;
    static const float ox[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const float oy[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    for (int i = 0; i < 8; i++)
        DrawTextEx(font, s, Vector2{x + ox[i] * thick, y + oy[i] * thick}, size, 0.f, edge);
    DrawTextEx(font, s, Vector2{x, y}, size, 0.f, ink);
}

// Red through yellow to green, like the ESP's health bar.
inline Color healthTint(double hp, double maxHp) {
    const float t = maxHp > 0.0 ? std::clamp((float)(hp / maxHp), 0.f, 1.f) : 0.f;
    return t < 0.5f ? rgba(255, (int)(510 * t), 40) : rgba((int)(510 * (1.f - t)), 230, 60);
}

// Whether any of the box's head, middle and feet lands inside the window, which
// is when the ESP is already showing this player and a pin would only repeat it.
inline bool boxOnScreen(const EntityData& e, const FMatrix& vp, int sw, int sh) {
    const double half = e.capsuleHalf > 20.f ? (double)e.capsuleHalf : 90.0;
    const FVector pts[3] = {
        e.origin,
        FVector(e.origin.X, e.origin.Y, e.origin.Z + half),
        FVector(e.origin.X, e.origin.Y, e.origin.Z - half),
    };
    for (const FVector& p : pts) {
        Vec2 s;
        if (worldToScreen(vp, p, s, sw, sh) && s.x >= 0.f && s.x <= (float)sw &&
            s.y >= 0.f && s.y <= (float)sh)
            return true;
    }
    return false;
}

inline float outlineW(float r) { return std::max(1.5f, r * 0.13f); }

// The room a pin needs between its centre and the window's edge when it points
// at that edge: the tip, the widest rim it is drawn with, and a little air.
inline float pinInset(float r) { return r * 2.0f + outlineW(r) * 3.8f + 3.f; }

// How far from the crosshair a pin sits along `u`: the ring's radius, cut short
// wherever the ring would leave the window. A large ring therefore flattens
// against the edges, top and bottom first, and at the slider's top becomes the
// window's border itself, so there is no jump between the two.
inline float placeDist(Vector2 u, float R, int sw, int sh, float inset) {
    const float hw = std::max(1.f, (float)sw * 0.5f - inset);
    const float hh = std::max(1.f, (float)sh * 0.5f - inset);
    float edge = 1e9f;
    if (std::fabs(u.x) > 1e-6f) edge = std::min(edge, hw / std::fabs(u.x));
    if (std::fabs(u.y) > 1e-6f) edge = std::min(edge, hh / std::fabs(u.y));
    return R >= kOofRadiusEdge ? edge : std::min(R, edge);
}

inline Vector2 outward(float ang) { return Vector2{std::sin(ang), -std::cos(ang)}; }

// Which way the pin points, in screen space (x right, y down).
inline Vector2 pointing(const EntityData& e, const ViewInfo& vi, const FMatrix& vp) {
    const double dx = e.origin.X - vi.Location.X;
    const double dy = e.origin.Y - vi.Location.Y;
    const double dz = e.origin.Z - vi.Location.Z;
    // The camera's axes, exactly as worldToScreen takes them from the matrix.
    const double fwd   = dx * vp.m[0][0] + dy * vp.m[0][1] + dz * vp.m[0][2];
    const double right = dx * vp.m[1][0] + dy * vp.m[1][1] + dz * vp.m[1][2];
    const double up    = dx * vp.m[2][0] + dy * vp.m[2][1] + dz * vp.m[2][2];

    const double rel = std::remainder(std::atan2(dy, dx) * 180.0 / PI - vi.Rotation.Yaw, 360.0)
                     * PI / 180.0;
    const Vector2 bearing{(float)std::sin(rel), (float)-std::cos(rel)};

    // worldToScreen puts the player at centre + (right, -up) * scale / depth,
    // with one scale for both axes, so the direction from the crosshair to
    // where they land is (right, -up) itself, whether that is on screen or off.
    const double len = std::sqrt(fwd * fwd + right * right + up * up);
    const double flat = std::sqrt(right * right + up * up);
    if (len < 1.0 || flat < 1e-6) return bearing;
    const Vector2 onView{(float)(right / flat), (float)(-up / flat)};

    // All projection from about 6 degrees in front of your shoulder line, all
    // bearing from about 3 degrees behind it, and a smooth blend between, so a
    // player walking round you never jumps from one to the other.
    double w = std::clamp((fwd / len + 0.05) / 0.15, 0.0, 1.0);
    w = w * w * (3.0 - 2.0 * w);
    const Vector2 v{(float)(w * onView.x + (1.0 - w) * bearing.x),
                    (float)(w * onView.y + (1.0 - w) * bearing.y)};
    const float vl = std::sqrt(v.x * v.x + v.y * v.y);
    return vl > 1e-4f ? Vector2{v.x / vl, v.y / vl} : bearing;
}

struct Pin {
    const EntityData* e = nullptr;
    float ang      = 0.f;   // radians from straight up, clockwise
    float trueAng  = 0.f;
    float r        = 0.f;   // disc radius, px
    float master   = 0.f;   // the indicator's opacity, 0..255
    float dim      = 1.f;   // what distance leaves of it, 0..1
    float fade     = 0.f;   // how far it has faded in, 0..1
    float alpha    = 0.f;   // all three together
    float labelW   = 0.f;   // the label behind it, px
    float labelH   = 0.f;
    bool  flashing = false;
    bool  flashOn  = false;
    int   cls      = -1;
    int   height   = 0;     // 1 above, -1 below
};

// How far in from its pin's centre a label's centre sits: on the crosshair's
// side, as far out as the label is wide in that direction, so it never touches
// the disc.
inline float labelBack(const Pin& p, Vector2 u) {
    return p.r + outlineW(p.r) + 4.f
         + std::fabs(u.x) * p.labelW * 0.5f + std::fabs(u.y) * p.labelH * 0.5f;
}

// Pins whose bearings nearly coincide would pile into one blob, and the distance
// behind each would print over its neighbour's. Neighbours on the ring are
// pushed apart until their discs clear each other, by as little as it takes and
// never more than a bounded amount, so every pin still points close to its
// player.
// `distAt(pin)` is how far out the pin sits at its current bearing, which on a
// flattened ring or the window's edge differs from bearing to bearing.
//
// The labels need room as much as the pins do. They sit further in, where the
// same angle spans fewer pixels, and a label is usually wider than its pin, so
// two pins that clear each other can still print their distances over one
// another. The spacing asked for is whichever of the two is larger.
template <class DistAt>
inline void declutter(std::vector<Pin>& pins, DistAt distAt) {
    const size_t n = pins.size();
    if (n < 2) return;
    std::sort(pins.begin(), pins.end(), [](const Pin& a, const Pin& b) { return a.ang < b.ang; });
    const float kGap = 6.f;
    const float kMaxShift = 30.f * PI / 180.f;
    auto angleFor = [](float chord, float dist) {
        return 2.f * std::asin(std::min(1.f, chord / (2.f * std::max(1.f, dist))));
    };
    // Half the label's span across the line from the crosshair: its width when
    // the pin is above or below the crosshair, its height when beside it.
    auto labelHalf = [](const Pin& p) {
        const Vector2 u = outward(p.ang);
        return 0.5f * (std::fabs(u.y) * p.labelW + std::fabs(u.x) * p.labelH);
    };
    auto need = [&](const Pin& a, const Pin& b) {
        const float da = distAt(a), db = distAt(b);
        const float discs = angleFor(a.r + b.r + kGap, 0.5f * (da + db));
        if (a.labelW <= 0.f && b.labelW <= 0.f) return discs;
        const float la = da - labelBack(a, outward(a.ang));
        const float lb = db - labelBack(b, outward(b.ang));
        return std::max(discs, angleFor(labelHalf(a) + labelHalf(b) + kGap, 0.5f * (la + lb)));
    };
    for (int it = 0; it < 16; it++) {
        bool moved = false;
        for (size_t i = 0; i < n; i++) {
            Pin& a = pins[i];
            Pin& b = pins[(i + 1) % n];
            float gap = b.ang - a.ang;
            if (i + 1 == n) gap += 2.f * PI;
            const float want = need(a, b);
            if (gap >= want) continue;
            const float push = (want - gap) * 0.5f;
            a.ang = std::clamp(a.ang - push, a.trueAng - kMaxShift, a.trueAng + kMaxShift);
            b.ang = std::clamp(b.ang + push, b.trueAng - kMaxShift, b.trueAng + kMaxShift);
            moved = true;
        }
        if (!moved) break;
    }
}

// A small up or down triangle, for a player a floor or more above or below.
inline void heightMark(float x, float cy, float s, int dir, Color ink, Color edge) {
    const float h = s * 0.9f;
    Vector2 a, b, c;
    if (dir > 0) {
        a = Vector2{x + s * 0.5f, cy - h * 0.5f};
        b = Vector2{x, cy + h * 0.5f};
        c = Vector2{x + s, cy + h * 0.5f};
    } else {
        a = Vector2{x + s * 0.5f, cy + h * 0.5f};
        b = Vector2{x + s, cy - h * 0.5f};
        c = Vector2{x, cy - h * 0.5f};
    }
    const float o = std::max(1.f, s * 0.18f);
    const Vector2 m{(a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f};
    auto grow = [&](Vector2 p) {
        const float dx = p.x - m.x, dy = p.y - m.y;
        const float l = std::sqrt(dx * dx + dy * dy);
        return l > 0.f ? Vector2{p.x + dx / l * o * 1.6f, p.y + dy / l * o * 1.6f} : p;
    };
    fillTri(grow(a), grow(b), grow(c), edge);
    fillTri(a, b, c, ink);
}

// `now` is seconds on any steady clock; it paces the fades and the flicker.
inline void draw(const Font& font, const EntityData* ents, int count, const ViewInfo& vi,
                 const FMatrix& vp, int sw, int sh, double now) {
    auto& tr = tracks();
    static double last = now;
    const float dt = (float)std::clamp(now - last, 0.0, 0.1);
    last = now;
    if (!g_oofEnabled || sw <= 0 || sh <= 0) {
        tr.clear();
        return;
    }

    // Height is measured against your own body, where the other player's is
    // measured too. The camera sits at your eyes, which would read everyone on
    // your own floor as below you.
    double myZ = vi.Location.Z - 70.0;
    for (int i = 0; i < count; i++)
        if (ents[i].valid && ents[i].isSelf) { myZ = ents[i].origin.Z; break; }

    const float maxDist = std::max(1.f, g_oofMaxDist);
    const float textSize = std::max(9.f, g_espTextSize);
    std::vector<Pin> pins;
    pins.reserve((size_t)count);
    for (int i = 0; i < count; i++) {
        const EntityData& e = ents[i];
        if (!e.valid || e.isSelf || e.isSpectator) continue;
        if (e.isTeammate && !g_oofTeammates) continue;
        if (e.distance > maxDist) continue;

        Track& t = tr[e.id ? e.id : (uintptr_t)(i + 1)];
        t.seen = now;

        // Eased both ways, so a player crossing the edge of the screen hands
        // over from box to pin instead of blinking. Out is quicker than in, so a
        // pin never lingers over a box that is already there.
        const bool off = !boxOnScreen(e, vp, sw, sh);
        const float step = (off ? 7.f : 12.f) * dt;
        t.fade = off ? std::min(1.f, t.fade + step) : std::max(0.f, t.fade - step);

        Pin p;
        p.e = &e;
        if (g_oofFlash && g_oofFlashDist > 0.f && e.distance < g_oofFlashDist) {
            const float closeness = 1.f - e.distance / g_oofFlashDist;   // 0 at the edge
            const float lo = std::max(0.2f, g_oofFlashMinHz);
            const float hi = std::max(lo, g_oofFlashMaxHz);
            // Each metre closer multiplies the rate by the same factor, which
            // reads as a steady speed-up. A linear ramp would spend most of its
            // change where a difference of a few flashes a second is least
            // noticeable.
            const float hz = lo * std::pow(hi / lo, closeness);
            t.phase += (double)hz * (double)dt;
            t.phase -= std::floor(t.phase);
            p.flashing = true;
            p.flashOn  = t.phase < 0.5;
        } else {
            t.phase = 0.0;          // entering the range lights it at once
        }
        if (t.fade <= 0.001f) continue;

        const Vector2 dir = pointing(e, vi, vp);
        p.ang = p.trueAng = std::atan2(dir.x, -dir.y);

        const float k = std::clamp(e.distance / maxDist, 0.f, 1.f);
        p.r = std::max(5.f, g_oofSize) * (g_oofScaleDist ? 1.f - 0.4f * k : 1.f);
        // Opacity is the whole indicator's, and distance takes a share of it
        // away, down to Far opacity's share at the max distance. A pin that is
        // flickering keeps all of it: a warning is never the one dimmed for
        // being far.
        const float farShare = std::clamp((float)g_oofFarAlpha / 255.f, 0.f, 1.f);
        p.master = std::clamp((float)g_oofAlpha, 0.f, 255.f);
        p.dim    = p.flashing ? 1.f : 1.f + (farShare - 1.f) * k;
        p.fade   = t.fade;
        p.alpha  = p.master * p.dim * p.fade;
        p.cls = classOf(e.maxHealth);
        if (g_oofHeight) {
            const double dz = (e.origin.Z - myZ) / 100.0;
            p.height = dz > g_oofHeightM ? 1 : (dz < -g_oofHeightM ? -1 : 0);
        }
        if (g_oofDistance || p.height != 0) {
            const float ts = textSize;
            float w = 0.f;
            if (g_oofDistance) {
                char buf[16];
                snprintf(buf, sizeof buf, "%.0fm", e.distance);
                w = MeasureTextEx(font, buf, ts, 0.f).x;
            }
            if (p.height != 0) w += (w > 0.f ? ts * 0.25f : 0.f) + ts * 0.7f;
            p.labelW = w;
            p.labelH = ts;
        }
        pins.push_back(p);
    }
    for (auto it = tr.begin(); it != tr.end();)
        it = (now - it->second.seen > 3.0) ? tr.erase(it) : std::next(it);
    if (pins.empty()) return;

    const float R = std::max(40.f, g_oofRadius);
    declutter(pins, [&](const Pin& p) {
        return placeDist(outward(p.ang), R, sw, sh, pinInset(p.r));
    });
    // Farthest first, so the nearest, most urgent pin is the one on top.
    std::sort(pins.begin(), pins.end(),
              [](const Pin& a, const Pin& b) { return a.e->distance > b.e->distance; });

    const Vector2 mid{(float)sw * 0.5f, (float)sh * 0.5f};
    for (const Pin& p : pins) {
        const EntityData& e = *p.e;
        const Vector2 u = outward(p.ang);
        const float r   = p.r;
        const float out = placeDist(u, R, sw, sh, pinInset(r));
        const Vector2 C{mid.x + u.x * out, mid.y + u.y * out};
        const float tip = r * 2.0f;
        const float o   = outlineW(r);
        const bool  lit = p.flashing && p.flashOn;
        const Color team = squadColor(e.squadIdx, false);
        const Color body = lit ? kNeon : team;
        const float a = p.alpha;

        if (lit)
            DrawCircleGradient(C, r * 2.2f, withAlpha(kNeon, a * 0.40f), withAlpha(kNeon, 0.f));

        // Outline: the same pin grown by the outline width all round. While it
        // is lit the rim takes the squad's colour, so which squad is on top of
        // you still shows through the flash.
        if (lit) {
            const float ro = o * 1.9f;
            fillPin(C, u, r + ro, tip + ro * tip / r, withAlpha(BLACK, a * 0.85f));
            fillPin(C, u, r + ro * 0.62f, tip + ro * 0.62f * tip / r, withAlpha(team, a));
        } else {
            fillPin(C, u, r + o, tip + o * tip / r, withAlpha(BLACK, a * 0.85f));
        }
        fillPin(C, u, r, tip, withAlpha(body, a));

        if (g_oofHealth && e.maxHealth > 0.0 && p.cls >= 0) {
            // Along the round back of the pin, from one flank to the other, so
            // it never crosses the tip.
            const float base = std::atan2(u.y, u.x) * 180.f / PI;
            const float half = std::acos(std::clamp(r / tip, -1.f, 1.f)) * 180.f / PI;
            const float from = base + half, span = 360.f - 2.f * half;
            const float t = std::clamp((float)(e.health / e.maxHealth), 0.f, 1.f);
            DrawRing(C, r + o * 0.2f, r + o + 1.5f, from, from + span, 32,
                     withAlpha(rgba(25, 25, 25), a));
            if (t > 0.f)
                DrawRing(C, r + o * 0.2f, r + o + 1.5f, from, from + span * t, 32,
                         withAlpha(healthTint(e.health, e.maxHealth), a));
        }

        if (g_oofLetters && p.cls >= 0) {
            static const char* kLetters[3] = {"L", "M", "H"};
            const char* s = kLetters[p.cls];
            const float fs = r * 1.3f;
            const Vector2 m = MeasureTextEx(font, s, fs, 0.f);
            // raylib's line box is taller than a capital, which leaves capitals
            // riding high when the box is centred; lowering by a few percent of
            // the size centres the letter itself.
            const float lx = C.x - m.x * 0.5f, ly = C.y - m.y * 0.5f + fs * 0.04f;
            // Dark ink on a light fill needs nothing else: the fill is the
            // contrast. A light outline around it would stack up over the thin
            // strokes and turn the letter pale.
            if (wantsDarkInk(body))
                DrawTextEx(font, s, Vector2{lx, ly}, fs, 0.f, withAlpha(rgba(10, 10, 10), a));
            else
                label(font, s, lx, ly, fs, withAlpha(WHITE, a), withAlpha(BLACK, a * 0.8f), 0.8f);
        }

        if (p.labelW > 0.f) {
            char buf[16] = "";
            if (g_oofDistance) snprintf(buf, sizeof buf, "%.0fm", e.distance);
            const float ts = p.labelH, w = p.labelW;
            const float mark = p.height != 0 ? ts * 0.7f : 0.f;
            const float back = labelBack(p, u);
            const float cx = C.x - u.x * back, cy = C.y - u.y * back;
            // Far pins dim, but their text only half as much: a faint pin is
            // the point, an unreadable number is not. Opacity still governs it.
            const float textA = p.master * (p.dim + (1.f - p.dim) * 0.5f) * p.fade;
            const Color ink  = withAlpha(lit ? kNeon : rgba(235, 235, 235), textA);
            const Color edge = withAlpha(BLACK, textA * 0.9f);
            if (buf[0]) label(font, buf, cx - w * 0.5f, cy - ts * 0.5f, ts, ink, edge);
            if (mark > 0.f) heightMark(cx + w * 0.5f - mark, cy, mark, p.height, ink, edge);
        }
    }
}

}  // namespace oof
