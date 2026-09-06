#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// State shared between the reader and render threads, and the
// runtime values the menu edits.
#include "structs.hpp"
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct EntityData {
    bool      valid       = false;
    // Head/feet in world space, from the skeleton. The bone array is
    // parent-relative, so only ROOT-PARENTED bones are usable without the
    // hierarchy - the head bone is one (large on-axis translation).
    bool      hasSkeleton = false;
    // Collision-capsule half height, straight from the game. Exact per class,
    // so the box needs no per-rig guessing. 0 = unavailable.
    float     capsuleHalf = 0.f;
    FVector   headWorld;
    FVector   feetWorld;
    bool      isSelf      = false;
    bool      isTeammate  = false;   // same SquadIndex as the local player
    bool      isSpectator = false;

    FVector   origin;   // world-space root location
    double    health    = 0.0;
    double    maxHealth = 100.0;
    int       squadIdx  = -1;
    std::string name;

    bool    hasHeadJoint = false;
    FVector headJoint{};
    bool    hasRig    = false;
    FVector boneHead{}, boneChest{}, bonePelvis{}, boneFeet{};
    float   eyeHeight = 0.f;
    // UPrimitiveComponent::LastRenderTime for this player's mesh, and the
    // verdict derived from it. `visible` is only meaningful when the offset has
    // been derived; with the offset 0 it stays true so nothing is filtered out.
    float   lastRenderTime = 0.f;
    bool    visible   = true;
    FVector velocity{};
    int     rigCount  = 0;
    FVector rigBones[128] = {};
    const void* rig   = nullptr;      // const skel::Rig*

    // Cached screen projections (filled by render side)
    Vec2    screenBones[80] = {};
    Vec2    screenOrigin = {};
    bool    onScreen    = false;

    float   distance    = 0.f; // metres (divide UE units by 100)
};

// Globals
inline constexpr int kMaxEntities = 64;

inline std::mutex       g_entityMtx;
inline EntityData       g_entities[kMaxEntities];
inline int              g_entityCount = 0;

inline ViewInfo         g_camView;    // camera snapshot (reader side writes)
inline std::mutex       g_camMtx;

inline std::atomic<bool> g_running { true };

// Feature flags (toggled at runtime)
inline bool g_espBoxes      = true;
inline bool g_espSnaplines  = false;  // line from screen bottom to each target
inline bool g_espBones      = false;
inline bool g_espHealth     = true;
inline bool g_espName       = true;
inline bool g_espDistance   = true;
inline bool g_espTeamColor  = true;   // different color for same squad
inline bool g_espTeammates  = false;  // draw squadmates too (off = enemies only)
inline bool g_espSelf       = false;  // draw a box on yourself
inline int  g_espAlpha      = 170;    // opacity of boxes and lines (0-255).
inline float g_boxHeadroom  = 0.07f;

// ── aim assist
inline bool  g_aimEnabled   = false;  // master toggle (hotkey + menu)
inline bool  g_aimHeld      = false;  // set by the X11 poll: fire button down
inline int   g_aimButton    = 0;      // 0 = RMB (ADS), 1 = LMB, 2 = either
inline int   g_aimBone      = 1;      // 0 head, 1 chest, 2 body, 3 legs
inline float g_aimSmooth    = 1.2f;   // 1 = instant snap, higher = slower pull
inline float g_aimFovPx     = 301.f;  // only engage within N px of the crosshair
inline float g_aimMaxDist   = 111.f;  // metres
inline int   g_aimTargetCnt = 0;      // diagnostics for the menu

inline float g_aimHeadLift  = 0.045f;

// ── the skeleton, as ONE feature
inline bool  g_espSkeleton  = false;
// and not for normal use. See render.hpp.
inline float g_aimTargetBias = 1.0f;

inline bool  g_aimSticky     = true;
inline float g_aimStickiness = 1.35f;   // challenger must be this much better
inline int   g_aimLockedIdx  = -1;      // runtime, not a setting

inline bool  g_aimDistFov    = true;

inline int   g_aimSmoothMode = 0;
inline float g_aimInertia    = 0.40f;
inline float g_aimInertiaAccX = 0.f, g_aimInertiaAccY = 0.f;   // runtime

inline float g_aimHeadFwd   = 0.020f;
inline float g_aimHeadUp    = 0.020f;

// ── triggerbot
inline bool  g_trigEnabled     = false;
inline int   g_trigButton      = 0;      // 0 = RMB held, 1 = always (no button)
inline bool  g_trigHeld        = false;  // runtime: button state from the poll
inline float g_trigForgiveness = 3.0f;   // px tolerance; sensitive slider
inline int   g_trigActivateMs  = 0;      // ms after the button is HELD before the
                                         // triggerbot arms (so ADS doesn't insta-fire)
inline int   g_trigDelayMs     = 0;      // ms ON-TARGET before firing (humanize)
inline int   g_trigHoldMs      = 25;     // how long the click is held
inline int   g_trigCooldownMs  = 45;     // min ms between shots (rate limit)
// already fires at the exact bone the aimbot targets.
inline bool  g_aimPredict   = true;
inline float g_aimLeadMs    = 35.f;    // ms of lead; ~ reader + present latency

// QUICK SCOPE. A sniper's recoil is part of aiming it, and an assist that keeps
// pulling through the shot fights that. With this on, firing hands control back
// immediately; the aim re-arms on the next ADS press, or after a delay if set.
inline bool  g_aimQuickScope     = false;
inline int   g_aimQuickRestoreMs = 0;      // 0 = only on the next ADS press
inline bool  g_lmbHeld           = false;  // runtime: fire button, polled
inline bool  g_aimSuppressed     = false;  // runtime: released by the shot
inline bool  g_trigSkeleton    = false;
inline int   g_trigSkelPart    = 0;      // 0 head, 1 chest, 2 body, 3 legs, 4 ALL body
inline bool  g_trigOnTarget    = false;  // runtime: is the crosshair on target now

// ── visibility (UPrimitiveComponent::LastRenderTime)
inline bool   g_visHave        = false;  // offset present AND calibrated
inline float  g_visMaxSeen     = 0.f;    // highest LastRenderTime observed
inline double g_visMaxWall     = 0.0;
inline float  g_visTolerance   = 0.12f;  // seconds of slack before "hidden"
inline int    g_visVisibleCnt  = 0;      // diagnostics for the menu
inline int    g_visHiddenCnt   = 0;
inline bool   g_aimVisibleOnly = false;  // aimbot: skip players behind cover
inline bool   g_trigVisibleOnly= false;  // trigger: hold fire on hidden players
inline int    g_visStyle       = 1;
inline int    g_visDimAlpha    = 60;      // alpha for players you cannot hit
inline float g_trigSizeScale   = 1.0f;   // 0 = pure pixels, 1 = true part size
// FLOOR. Size-scaling is right in principle but collapses at range: a head 8 px
// tall gives 8*0.06 = 0.48 px of tolerance, finer than the projection's own
// jitter, so far shots can never land. Never go stricter than this many pixels.
inline float g_trigMinTol      = 2.0f;
inline float g_trigOnTargetTol = 0.f;    // runtime: the tolerance used this frame
inline float g_trigOnTargetPx  = 0.f;    // runtime: measured offset this frame

// ── menu window size (resizable, persisted)
inline float g_menuW = 300.f;
inline float g_menuH = 320.f;

inline constexpr float kHeadHalf     = 0.06f;   // joint -> centre of the skull
inline constexpr float kHeadCapsuleT = 0.892f;  // measured eye height, as a fraction

// ── aim debug, for the menu
inline bool  g_aimDebug     = false;  // draw the aim point and its source
inline int   g_aimSkelCnt   = 0;      // targets in range WITH a skeleton
inline int   g_aimNoSkelCnt = 0;      // targets in range WITHOUT one
inline bool  g_aimLockSkel  = false;  // is the CURRENT lock using bone data?
inline float g_aimLockPx    = 0.f;    // pixels from crosshair to the aim point
inline Vec2  g_aimLockPt    = {};     // where it is aiming, in screen space
inline float g_aimLockDist  = 0.f;    // metres to the locked target
inline float g_maxEspDist   = 300.f; // metres
// Camera FOV. PlayerCameraManager reads NULL on this build so the real POV FOV
// is unreachable; the camera is derived from ControlRotation + BaseEyeHeight
// instead. Match this to your in-game FOV setting or the ESP will drift.
inline float g_fov          = 90.f;
// g_fovScale lives in structs.hpp (used by worldToScreen there).
// Menu visibility, toggled with INSERT alongside click-through. When the menu
// is not interactive there is no reason to occupy the screen with it, so the
// two are driven together rather than independently.
inline std::atomic<bool> g_menuVisible{false};
