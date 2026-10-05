#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Each player's own silhouette, traced round its edge.
//
// The posed vertices from body.hpp are the surface of the body as the game
// draws it, sampled every centimetre or two. Each one is drawn as a dot the
// size of that gap at its own range, into a mask the size of the screen, which
// fills the body in solid. The mask is then drawn back over the game through a
// shader that keeps only a band of pixels just outside the filled shape, in the
// player's colour, and an optional faint fill inside it. What is left is the
// edge of the body itself: an arm held out, a crouch, a weapon raised all show
// as they are, where a box only ever shows how tall someone is.
//
// A player whose mesh could not be read, because the game keeps no copy of its
// vertices, is drawn from the tubes on their bones instead: the same shapes the
// trigger falls back to, filled into the same mask and traced the same way. It
// is a person's proportions rather than their exact surface, but it bends and
// crouches with them.
#include <algorithm>
#include <cmath>
#include <vector>
#include <raylib.h>
#include <rlgl.h>
#include "body.hpp"
#include "structs.hpp"

namespace outline {

struct Item {
    const body::Body* body = nullptr;   // the mesh, when it has been read
    const EntityData* ent  = nullptr;   // otherwise the tubes on their bones
    Color col{};                        // the player's ESP colour, alpha included
};

struct State {
    RenderTexture2D mask{};
    Shader edge{};
    int  locTexel = -1, locWidth = -1, locFill = -1;
    bool shaderTried = false, shaderOk = false;
};
inline State& state() { static State s; return s; }

// Outside the body: the strongest pixel of body within `width` on a few rings,
// so the band is as wide as asked whichever way the edge runs. Inside: the
// body's own colour at `fill`, or nothing.
inline const char* kEdge330 = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texel;
uniform float width;
uniform float fill;
out vec4 finalColor;
void main() {
    vec4 c = texture(texture0, fragTexCoord);
    if (c.a > 0.0) {
        if (fill <= 0.0) discard;
        finalColor = vec4(c.rgb, c.a * fill);
        return;
    }
    vec4 best = vec4(0.0);
    for (int ring = 1; ring <= 3; ring++) {
        float r = width * float(ring) / 3.0;
        for (int i = 0; i < 16; i++) {
            float a = 6.28318530718 * (float(i) + 0.5 * float(ring)) / 16.0;
            vec4 s = texture(texture0, fragTexCoord + vec2(cos(a), sin(a)) * r * texel);
            if (s.a > best.a) best = s;
        }
    }
    if (best.a <= 0.0) discard;
    finalColor = best;
}
)";

inline const char* kEdge120 = R"(#version 120
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texel;
uniform float width;
uniform float fill;
void main() {
    vec4 c = texture2D(texture0, fragTexCoord);
    if (c.a > 0.0) {
        if (fill <= 0.0) discard;
        gl_FragColor = vec4(c.rgb, c.a * fill);
        return;
    }
    vec4 best = vec4(0.0);
    for (int ring = 1; ring <= 3; ring++) {
        float r = width * float(ring) / 3.0;
        for (int i = 0; i < 16; i++) {
            float a = 6.28318530718 * (float(i) + 0.5 * float(ring)) / 16.0;
            vec4 s = texture2D(texture0, fragTexCoord + vec2(cos(a), sin(a)) * r * texel);
            if (s.a > best.a) best = s;
        }
    }
    if (best.a <= 0.0) discard;
    gl_FragColor = best;
}
)";

inline void ensure(int sw, int sh) {
    State& s = state();
    if (s.mask.id == 0 || s.mask.texture.width != sw || s.mask.texture.height != sh) {
        if (s.mask.id != 0) UnloadRenderTexture(s.mask);
        s.mask = LoadRenderTexture(sw, sh);
        SetTextureFilter(s.mask.texture, TEXTURE_FILTER_POINT);
    }
    if (!s.shaderTried) {
        s.shaderTried = true;
        s.edge = LoadShaderFromMemory(nullptr,
                                      rlGetVersion() == RL_OPENGL_21 ? kEdge120 : kEdge330);
        s.shaderOk = s.edge.id != 0 && s.edge.id != rlGetShaderIdDefault();
        if (s.shaderOk) {
            s.locTexel = GetShaderLocation(s.edge, "texel");
            s.locWidth = GetShaderLocation(s.edge, "width");
            s.locFill  = GetShaderLocation(s.edge, "fill");
        } else {
            printf("[outline] the edge shader did not build, so bodies are drawn "
                   "filled and faint instead of outlined\n");
            fflush(stdout);
        }
    }
}

inline void unload() {
    State& s = state();
    if (s.mask.id != 0) UnloadRenderTexture(s.mask);
    if (s.shaderOk) UnloadShader(s.edge);
    s = State{};
}

// The part of the screen something was drawn into.
struct Span { float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool any() const { return x1 >= x0; } };

// First pass: every body filled in solid, in its own colour, into the mask.
inline Span fillMask(const std::vector<Item>& items, const FMatrix& vp, int sw, int sh) {
    Span span;
    if (items.empty() || sw <= 0 || sh <= 0) return span;
    ensure(sw, sh);
    State& s = state();
    if (s.mask.id == 0) return span;

    // The same projection the ESP draws with, written out once so a body of
    // thousands of points does not call it thousands of times.
    float fov = vp.m[3][3] * g_fovScale;
    if (fov < 1.f || fov > 170.f) fov = 90.f * g_fovScale;
    const float cx = sw * 0.5f, cy = sh * 0.5f;
    const float scale = cy / tanf(fov * 3.14159265f / 360.f);
    const float ox = vp.m[3][0], oy = vp.m[3][1], oz = vp.m[3][2];
    const float f0 = vp.m[0][0], f1 = vp.m[0][1], f2 = vp.m[0][2];
    const float r0 = vp.m[1][0], r1 = vp.m[1][1], r2 = vp.m[1][2];
    const float u0 = vp.m[2][0], u1 = vp.m[2][1], u2 = vp.m[2][2];

    float& x0 = span.x0; float& y0 = span.y0; float& x1 = span.x1; float& y1 = span.y1;
    BeginTextureMode(s.mask);
    ClearBackground(BLANK);
    // Written, not blended: the mask has to hold exactly the colour and the
    // alpha of whatever body was drawn last at each pixel.
    rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    rlDisableBackfaceCulling();
    // A batch of quads keeps whatever texture the last one used, which can be
    // the font; a dot sampling a glyph's corner would come out see-through.
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    rlTexCoord2f(0.f, 0.f);
    for (const Item& it : items) {
        if (!it.body || !it.body->model || it.col.a == 0) continue;
        const body::Body& b = *it.body;
        const body::Model& m = *b.model;
        // How far apart neighbouring vertices land on screen, from the middle
        // of the body. Once they crowd under a pixel, every one of them is one
        // more dot on the same pixel: take every k-th and make each dot cover
        // what the skipped ones would have.
        const float mx = 0.5f * (b.lo[0] + b.hi[0]) - ox;
        const float my = 0.5f * (b.lo[1] + b.hi[1]) - oy;
        const float mz = 0.5f * (b.lo[2] + b.hi[2]) - oz;
        const float midDepth = mx * f0 + my * f1 + mz * f2;
        int step = 1;
        if (midDepth > 1.f) {
            const float gapPx = m.spacing * scale / midDepth;
            if (gapPx < 1.f) step = std::clamp((int)(1.f / (gapPx * gapPx)), 1, 16);
        }
        if ((int)m.rad.size() != m.verts) continue;
        const float grow = std::sqrt((float)step);
        rlColor4ub(it.col.r, it.col.g, it.col.b, it.col.a);
        const float* X = b.xyz.data();
        for (int v = 0; v < m.verts; v += step) {
            const float dx = X[v * 3] - ox, dy = X[v * 3 + 1] - oy, dz = X[v * 3 + 2] - oz;
            const float depth = dx * f0 + dy * f1 + dz * f2;
            if (depth < 1.f) continue;
            const float inv = scale / depth;
            const float sx = cx + (dx * r0 + dy * r1 + dz * r2) * inv;
            const float sy = cy - (dx * u0 + dy * u1 + dz * u2) * inv;
            // Each dot as big as the surface its vertex stands for.
            const float r = std::max(0.75f, m.rad[v] * grow * inv);
            if (sx + r < 0.f || sx - r > sw || sy + r < 0.f || sy - r > sh) continue;
            rlVertex2f(sx - r, sy - r);
            rlVertex2f(sx - r, sy + r);
            rlVertex2f(sx + r, sy + r);
            rlVertex2f(sx + r, sy - r);
            x0 = std::min(x0, sx - r); x1 = std::max(x1, sx + r);
            y0 = std::min(y0, sy - r); y1 = std::max(y1, sy + r);
        }
    }
    rlEnd();

    // Players without a mesh: the tubes on their bones, each as it projects, a
    // circle at either end and the band between their edges. A tube seen end
    // on is just its two circles, which is what it looks like.
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_TRIANGLES);
    rlTexCoord2f(0.f, 0.f);
    hit::Capsule caps[192];
    auto proj = [&](const FVector& p, float& sx, float& sy, float& inv) {
        const float dx = (float)p.X - ox, dy = (float)p.Y - oy, dz = (float)p.Z - oz;
        const float depth = dx * f0 + dy * f1 + dz * f2;
        if (depth < 1.f) return false;
        inv = scale / depth;
        sx = cx + (dx * r0 + dy * r1 + dz * r2) * inv;
        sy = cy - (dx * u0 + dy * u1 + dz * u2) * inv;
        return true;
    };
    auto disc = [&](float x, float y, float r) {
        const int n = std::clamp((int)(r * 0.8f), 8, 32);
        for (int k = 0; k < n; k++) {
            const float a0 = 6.2831853f * k / n, a1 = 6.2831853f * (k + 1) / n;
            rlVertex2f(x, y);
            rlVertex2f(x + r * std::cos(a0), y + r * std::sin(a0));
            rlVertex2f(x + r * std::cos(a1), y + r * std::sin(a1));
        }
        x0 = std::min(x0, x - r); x1 = std::max(x1, x + r);
        y0 = std::min(y0, y - r); y1 = std::max(y1, y + r);
    };
    for (const Item& it : items) {
        if (it.body || !it.ent || it.col.a == 0) continue;
        int n = hit::capsules(*it.ent, hit::pAll, 1.0, caps, 192);
        if (n == 0) n = hit::bodyCapsule(*it.ent, 1.0, caps);
        rlColor4ub(it.col.r, it.col.g, it.col.b, it.col.a);
        for (int i = 0; i < n; i++) {
            float ax, ay, ai, bx, by, bi;
            if (!proj(caps[i].a, ax, ay, ai) || !proj(caps[i].b, bx, by, bi)) continue;
            const float ra = std::max(0.75f, (float)caps[i].r * ai);
            const float rb = std::max(0.75f, (float)caps[i].r * bi);
            disc(ax, ay, ra);
            disc(bx, by, rb);
            const float ex = bx - ax, ey = by - ay;
            const float len = std::sqrt(ex * ex + ey * ey);
            if (len < 0.5f) continue;
            const float nx = -ey / len, ny = ex / len;
            rlVertex2f(ax + nx * ra, ay + ny * ra);
            rlVertex2f(ax - nx * ra, ay - ny * ra);
            rlVertex2f(bx - nx * rb, by - ny * rb);
            rlVertex2f(ax + nx * ra, ay + ny * ra);
            rlVertex2f(bx - nx * rb, by - ny * rb);
            rlVertex2f(bx + nx * rb, by + ny * rb);
        }
    }
    rlEnd();
    rlSetTexture(0);
    // Culling is switched on the GPU at once, but what was drawn above only
    // reaches it when the batch is flushed. Flushed first, so all of it is drawn
    // with culling off, whichever way round each shape happened to be wound.
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndBlendMode();
    EndTextureMode();
    return span;
}

// Second pass: the mask back over whatever is being drawn to, traced. Only the
// part of the screen with a body in it, plus the band, is run through the
// shader, so a player far away costs a few pixels rather than the whole screen.
inline void composite(const Span& span, int sw, int sh, float width, int fill) {
    State& s = state();
    if (!span.any() || s.mask.id == 0) return;
    const float pad = width + 2.f;
    const float rx = std::max(0.f, std::floor(span.x0 - pad));
    const float ry = std::max(0.f, std::floor(span.y0 - pad));
    const float rw = std::min((float)sw, std::ceil(span.x1 + pad)) - rx;
    const float rh = std::min((float)sh, std::ceil(span.y1 + pad)) - ry;
    if (rw <= 0.f || rh <= 0.f) return;
    // A render texture is stored bottom up, so the source rectangle is taken
    // from the bottom and turned over.
    const Rectangle src{ rx, (float)sh - ry - rh, rw, -rh };
    if (s.shaderOk) {
        BeginShaderMode(s.edge);
        const float texel[2] = { 1.f / (float)sw, 1.f / (float)sh };
        const float w = std::clamp(width, 0.5f, 12.f);
        const float f = std::clamp(fill, 0, 255) / 255.f;
        SetShaderValue(s.edge, s.locTexel, texel, SHADER_UNIFORM_VEC2);
        SetShaderValue(s.edge, s.locWidth, &w, SHADER_UNIFORM_FLOAT);
        SetShaderValue(s.edge, s.locFill, &f, SHADER_UNIFORM_FLOAT);
        DrawTextureRec(s.mask.texture, src, Vector2{ rx, ry }, WHITE);
        EndShaderMode();
    } else {
        DrawTextureRec(s.mask.texture, src, Vector2{ rx, ry }, Fade(WHITE, 0.35f));
    }
}

// `width` is the band's width in pixels; `fill` is how much of the body's
// colour shows inside it, out of 255.
inline void draw(const std::vector<Item>& items, const FMatrix& vp, int sw, int sh,
                 float width, int fill) {
    composite(fillMask(items, vp, sw, sh), sw, sh, width, fill);
}

}  // namespace outline
