#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// The one text face the tool draws with, in the windows that need it.
#include <raylib.h>
#include <cstdio>

// One atlas at 32 px serves every size: label sizes are runtime settings and a
// bilinear-filtered atlas scales down cleanly. Arch first, Debian second, so the
// common case does not print a load failure for a path that never existed.
inline Font loadUiFont() {
    static const char* kFonts[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    };
    for (const char* p : kFonts) {
        if (!FileExists(p)) continue;
        Font f = LoadFontEx(p, 32, nullptr, 0);
        if (f.texture.id != 0) {
            SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
            printf("[font] %s\n", p);
            return f;
        }
    }
    printf("[font] WARNING: no DejaVuSans.ttf found; the built-in bitmap font\n"
           "       is small and blocky but will do.\n");
    return GetFontDefault();
}
