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

inline constexpr uint32_t kSharedMagic  = 0x554D4252;   // 'UMBR'
inline constexpr int      kSharedFields = 128;

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

// True when the mapping holds settings this process has not applied yet.
inline bool sharedPull(SharedState* sh, uint32_t& seenGen) {
    const uint32_t gen = sh->settingsGen.load(std::memory_order_acquire);
    if (gen == seenGen) return false;
    sharedReadFields(sh);
    seenGen = gen;
    return true;
}

// Publishes this process's settings if any of them differ from the mapping.
inline bool sharedPushIfChanged(SharedState* sh, uint32_t& seenGen) {
    uint32_t n = 0;
    bool changed = false;
    for (auto& kv : settingFields()) {
        if (n >= kSharedFields) break;
        const SharedState::Field& f = sh->fields[n++];
        switch (kv.second.kind) {
            case SettingRef::BOOL:  changed |= ((*(bool*) kv.second.p ? 1 : 0) != f.i); break;
            case SettingRef::INT:   changed |= (*(int*)   kv.second.p != f.i);          break;
            case SettingRef::FLOAT: changed |= (*(float*) kv.second.p != f.f);          break;
        }
        if (changed) break;
    }
    if (!changed) return false;
    sharedWriteFields(sh);
    seenGen = sh->settingsGen.fetch_add(1, std::memory_order_release) + 1;
    return true;
}
