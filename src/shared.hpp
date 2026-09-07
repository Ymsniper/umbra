#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// State shared between the overlay process and the menu process.
//
// The two run separately on purpose. A window manager only performs a real
// activation switch between windows it manages, and that switch is what makes
// the game release the pointer it grabs for mouse-look. An overlay has to be
// override-redirect to stay click-through and correctly placed, which puts it
// outside the window manager entirely -- so it can never be the window that
// takes activation, no matter how its input focus is set. Splitting them lets
// the overlay stay unmanaged and untouched while the menu is an ordinary
// window that activates, and deactivates, exactly like any other application.
//
// The mapping is created before fork(), so both processes inherit it and no
// name, file or cleanup is involved.
#include "global.hpp"
#include "settings.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

inline constexpr uint32_t kSharedMagic   = 0x554D4252;   // 'UMBR'
inline constexpr int      kSharedFields  = 128;
inline constexpr int      kSharedBlips   = 64;

// One enemy on the sonar. The offset is in world units from the local player,
// unrotated: the sonar turns it into screen space itself, so the reader does
// not have to know how the sonar is oriented or scaled.
struct SonarBlip {
    float   dx = 0.f, dy = 0.f;
    int32_t squad = -1;
    uint8_t cls   = 3;       // 0 light, 1 medium, 2 heavy, 3 unknown
};

struct SonarFrame {
    float   yaw   = 0.f;     // where the local player is facing, degrees
    int32_t count = 0;
    SonarBlip blips[kSharedBlips];
};

// Light 150, Medium 250, Heavy 350. Nearest wins, so a health buff or a class
// that is not one of the three still lands on something sensible.
inline uint8_t classFromMaxHealth(double maxHp) {
    if (maxHp < 50.0) return 3;
    const double table[3] = { 150.0, 250.0, 350.0 };
    uint8_t best = 3;
    double bestErr = 1e18;
    for (uint8_t i = 0; i < 3; i++) {
        const double err = maxHp > table[i] ? maxHp - table[i] : table[i] - maxHp;
        if (err < bestErr) { bestErr = err; best = i; }
    }
    return best;
}

// Everything the menu shows that is not itself a setting.
struct Status {
    int32_t  entityCount    = 0;
    int32_t  aimTargetCnt   = 0;
    int32_t  visVisibleCnt  = 0;
    int32_t  visHiddenCnt   = 0;
    int32_t  fps            = 0;
    float    trigOnTargetPx = 0.f;
    float    trigOnTargetTol= 0.f;
    float    camFov         = 0.f;
    uint64_t kmodOk         = 0;
    uint64_t kmodFellBack   = 0;
    uint8_t  aimHeld        = 0;
    uint8_t  aimSuppressed  = 0;
    uint8_t  trigHeld       = 0;
    uint8_t  trigOnTarget   = 0;
    uint8_t  visHave        = 0;
    uint8_t  haveLastRenderTime = 0;
    uint8_t  usingKmod      = 0;
    uint8_t  vmouseReady    = 0;
    uint8_t  vmouseKernel   = 0;
};

struct SharedState {
    uint32_t magic;
    uint32_t fieldCount;
    int32_t  gamePid;

    // Settings, in settingFields() order, which is sorted by name and so is
    // identical in both processes. Either side may write; the generation says
    // whose copy is newer.
    std::atomic<uint32_t> settingsGen;
    struct Field { int32_t kind; int32_t i; float f; };
    Field fields[kSharedFields];

    // Written by the overlay, read by the menu.
    std::atomic<uint32_t> statusGen;
    Status status;

    // Written by the overlay, read by the sonar.
    std::atomic<uint32_t> sonarGen;
    SonarFrame sonar;

    std::atomic<uint8_t> menuVisible;
};

// Creates the mapping. Must be called BEFORE fork().
inline SharedState* sharedCreate() {
    void* p = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto* sh = new (p) SharedState();
    std::memset(sh->fields, 0, sizeof(sh->fields));
    sh->magic      = kSharedMagic;
    sh->fieldCount = 0;
    sh->gamePid    = 0;
    sh->settingsGen.store(0);
    sh->statusGen.store(0);
    sh->sonarGen.store(0);
    sh->menuVisible.store(0);
    return sh;
}

// Copies this process's settings into the mapping. Returns the field count.
inline uint32_t sharedWriteFields(SharedState* sh) {
    uint32_t n = 0;
    for (auto& kv : settingFields()) {
        if (n >= kSharedFields) break;
        SharedState::Field f{};
        switch (kv.second.kind) {
            case SettingRef::BOOL:  f.kind = 0; f.i = *(bool*) kv.second.p ? 1 : 0; break;
            case SettingRef::INT:   f.kind = 1; f.i = *(int*)  kv.second.p;         break;
            case SettingRef::FLOAT: f.kind = 2; f.f = *(float*)kv.second.p;         break;
        }
        sh->fields[n++] = f;
    }
    sh->fieldCount = n;
    return n;
}

// Copies the mapping's settings into this process's globals.
inline void sharedReadFields(const SharedState* sh) {
    uint32_t n = 0;
    for (auto& kv : settingFields()) {
        if (n >= sh->fieldCount || n >= kSharedFields) break;
        const SharedState::Field& f = sh->fields[n++];
        switch (kv.second.kind) {
            case SettingRef::BOOL:  *(bool*) kv.second.p = (f.i != 0); break;
            case SettingRef::INT:   *(int*)  kv.second.p = f.i;        break;
            case SettingRef::FLOAT: *(float*)kv.second.p = f.f;        break;
        }
    }
}

inline SharedState::Field fieldOf(const SettingRef& ref) {
    SharedState::Field f{};
    switch (ref.kind) {
        case SettingRef::BOOL:  f.kind = 0; f.i = *(bool*) ref.p ? 1 : 0; break;
        case SettingRef::INT:   f.kind = 1; f.i = *(int*)  ref.p;         break;
        case SettingRef::FLOAT: f.kind = 2; f.f = *(float*)ref.p;         break;
    }
    return f;
}

inline bool fieldSame(const SharedState::Field& a, const SharedState::Field& b) {
    return a.kind == b.kind && a.i == b.i && a.f == b.f;
}

// What this process last agreed the settings were. Publishing the whole set on
// every frame would mean the busiest process constantly overwrote edits made in
// another one; comparing against this says which fields THIS process changed,
// so each side publishes only its own and the rest merge.
struct SettingsMirror {
    uint32_t gen = 0;
    uint32_t count = 0;
    SharedState::Field snap[kSharedFields];
};

inline void mirrorCapture(SettingsMirror& m) {
    uint32_t n = 0;
    for (auto& kv : settingFields()) {
        if (n >= kSharedFields) break;
        m.snap[n] = fieldOf(kv.second);
        n++;
    }
    m.count = n;
}

// Take anything newer from the mapping, then publish whatever changed here.
inline void sharedSync(SharedState* sh, SettingsMirror& m) {
    const uint32_t gen = sh->settingsGen.load(std::memory_order_acquire);
    if (gen != m.gen) {
        sharedReadFields(sh);
        m.gen = gen;
        mirrorCapture(m);
    }

    uint32_t n = 0;
    bool published = false;
    for (auto& kv : settingFields()) {
        if (n >= kSharedFields) break;
        const SharedState::Field cur = fieldOf(kv.second);
        if (!fieldSame(cur, m.snap[n])) {
            sh->fields[n] = cur;
            m.snap[n]     = cur;
            published     = true;
        }
        n++;
    }
    if (published)
        m.gen = sh->settingsGen.fetch_add(1, std::memory_order_release) + 1;
}
