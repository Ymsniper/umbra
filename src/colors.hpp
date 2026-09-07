#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Squad colours, shared by the overlay and the sonar so a player is the
// same colour in both.
#include <raylib.h>

// Builds a raylib Color from ints, clamped. Channels here are computed from
// alphas and ratios, so one clamping helper keeps the narrowing in one place.
inline Color rgba(int r, int g, int b, int a = 255) {
    auto c8 = [](int v) -> unsigned char {
        return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return Color{c8(r), c8(g), c8(b), c8(a)};
}

// The sonar draws small marks on a dark disc, where the deeper palette colours
// sit too close to the background to pick out at a glance. Each is lifted
// toward white by how dark it actually is, so the dim ones gain a lot and the
// already-bright ones barely move. A flat lift would wash the bright colours
// together and make two squads harder to tell apart, which is worse than the
// problem it solves. Hue is untouched, so a squad is the colour it is on the
// ESP, only louder.
inline Color neon(Color c, float strength = 0.45f) {
    const float luma = (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.f;
    const float lift = strength * (1.f - luma);
    auto up = [&](unsigned char v) { return (int)(v + (255 - v) * lift + 0.5f); };
    return rgba(up(c.r), up(c.g), up(c.b), c.a);
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
