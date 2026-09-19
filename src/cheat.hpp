#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Reader thread. Resolves the local player and the GameState, then
// fills the shared entity list the render side draws from.
#include "mem.hpp"
#include "offsets.hpp"
#include "runtime_offsets.hpp"
#include "skeleton.hpp"
#include "structs.hpp"
#include "global.hpp"
#include "gobjects_direct.hpp"
#include <cstdio>
#include <chrono>
#include <map>
#include <ctime>
#include <cmath>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>

// UWorld auto-discovery via RVA_FN_GET_WORLD_FROM_CTX

static constexpr size_t kScanBytes = 0x600;

inline uintptr_t findGWorld(const Mem& mem) {
    using namespace offsets;

    // export UWORLD_ADDR=0x<addr>
    {
        const char* env = getenv("UWORLD_ADDR");
        if (env && *env) {
            uintptr_t addr = (uintptr_t)strtoull(env, nullptr, 16);
            if (addr && addr > 0x10000 && addr < 0x7FFFFFFFFFFFULL) {
                uintptr_t vtable = addr ? mem.readPtr(addr) : 0;
                if (mem.vtableInModule(vtable)) {
                    printf("[gworld] UWORLD_ADDR=0x%lx  vtable=0x%lx  OK - using directly.\n",
                           addr, vtable);
                    return addr;
                }
                printf("[gworld] UWORLD_ADDR=0x%lx  vtable=0x%lx  INVALID - ignoring env var.\n",
                       addr, vtable);
            }
        }
    }

    if (GWorldOffset != 0) {
        uintptr_t ptr    = mem.readPtr(mem.modbase + GWorldOffset);
        uintptr_t vtable = ptr ? mem.readPtr(ptr) : 0;
        if (ptr && mem.vtableInModule(vtable)) {
            printf("[gworld] pre-set offset 0x%lx  →  UWorld = 0x%lx  (vtable OK)\n",
                   GWorldOffset, ptr);
            return ptr;
        }
        // Offset is stale (game updated) - fall through to auto-scan
        printf("[gworld] pre-set offset 0x%lx  →  0x%lx  INVALID (vtable=0x%lx)\n",
               GWorldOffset, ptr, vtable);
        printf("[gworld] GWorldOffset is stale - engaging auto-scan\n");
    }

    // ── auto-discovery via function RVA
    printf("[gworld] GWorldOffset not set - scanning fn @ modbase+0x%lx\n",
           RVA_FN_GET_WORLD_FROM_CTX);

    uintptr_t fnAddr = mem.modbase + RVA_FN_GET_WORLD_FROM_CTX;

    std::vector<uint8_t> buf(kScanBytes);
    if (!mem.read_raw(fnAddr, buf.data(), buf.size())) {
        printf("[gworld] ERROR: cannot read function bytes at 0x%lx\n", fnAddr);
        printf("         Is RVA_FN_GET_WORLD_FROM_CTX correct for this build?\n");
        return 0;
    }

    for (size_t i = 0; i + 7 <= buf.size(); ++i) {
        if (buf[i]   != 0x48) continue;            // REX.W
        if (buf[i+1] != 0x8B) continue;            // MOV
        if ((buf[i+2] & 0xC7) != 0x05) continue;   // mod=00, rm=101 (rip-relative)

        int32_t disp;
        memcpy(&disp, &buf[i+3], 4);

        uintptr_t nextInstr  = fnAddr + i + 7;
        uintptr_t targetAddr = (uintptr_t)((int64_t)nextInstr + disp);

        // targetAddr is the storage location of the UWorld global pointer
        uintptr_t candidate = mem.readPtr(targetAddr);

        // Basic pointer sanity
        if (!candidate || candidate < 0x10000 || candidate > 0x7FFFFFFFFFFFULL) continue;
        if (candidate % 8 != 0) continue;  // UObjects are 8-byte aligned

        // Hop 1: candidate's vtable should be inside the game module
        uintptr_t vtable = mem.readPtr(candidate);
        if (!mem.vtableInModule(vtable)) continue;

        // Hop 2: UWorld->OwningGameInstance should also be a vtable'd object
        uintptr_t gi = mem.readPtr(candidate + UWorld::OwningGameInstance);
        if (!gi || gi < 0x10000 || gi > 0x7FFFFFFFFFFFULL) continue;
        uintptr_t giVtable = mem.readPtr(gi);
        if (!mem.vtableInModule(giVtable)) continue;

        // Both hops pass - this is almost certainly UWorld
        uintptr_t resolvedOffset = targetAddr - mem.modbase;
        printf("[gworld] auto-found: fn+0x%03zx  →  storage @ modbase+0x%lx\n",
               i, resolvedOffset);
        printf("[gworld] UWorld = 0x%lx  (vtable=0x%lx  gi=0x%lx)\n",
               candidate, vtable, gi);
        printf("[gworld] TIP: set GWorldOffset = 0x%lx in offsets.hpp to skip scan next time\n",
               resolvedOffset);
        return candidate;
    }

    printf("[gworld] auto-discovery failed - no valid candidate found.\n");
    printf("         Run ./find_uworld for detailed diagnostics.\n");
    printf("         Or: sudo scanmem %d  then search for UWorld address\n", mem.pid);
    return 0;
}

// UTF-16LE FString → std::string
inline std::string readFString(const Mem& mem, uintptr_t addr) {
    uintptr_t dataPtr = mem.readPtr(addr + offsets::FString_Data);
    int32_t   len     = mem.read<int32_t>(addr + offsets::FString_Len);
    if (!dataPtr || len <= 0 || len > 256) return "";

    std::vector<uint16_t> buf(len);
    mem.read_raw(dataPtr, buf.data(), len * sizeof(uint16_t));

    std::string out;
    out.reserve(len);
    for (auto c : buf) {
        if (!c) break;
        if      (c < 0x80)  { out += (char)c; }
        else if (c < 0x800) { out += (char)(0xC0|(c>>6)); out += (char)(0x80|(c&0x3F)); }
        else                 { out += (char)(0xE0|(c>>12)); out += (char)(0x80|((c>>6)&0x3F)); out += (char)(0x80|(c&0x3F)); }
    }
    return out;
}

// Bone reading
inline void readBones(const Mem& mem, uintptr_t pawn, EntityData& ent) {
    using namespace offsets;
    ent.hasSkeleton = false;

    // Collision capsule first - it is the game's own per-class height and needs
    // no bone data at all.
    uintptr_t cap = (g_off.ACharacter_CapsuleComponent && g_off.Capsule_HalfHeight)
                  ? mem.readPtr(pawn + g_off.ACharacter_CapsuleComponent) : 0;
    if (cap) {
        float hh = mem.read<float>(cap + g_off.Capsule_HalfHeight);
        if (hh > 20.f && hh < 300.f) ent.capsuleHalf = hh;   // sane range only
    }

    // The game's own eye height for this pawn. Reflected, replicated, and exact.
    if (g_off.APawn_BaseEyeHeight) {
        const float eh = mem.read<float>(pawn + g_off.APawn_BaseEyeHeight);
        if (std::isfinite(eh) && eh > 10.f && eh < 200.f) ent.eyeHeight = eh;
    }

    if (!g_off.APawn_Mesh) return;
    uintptr_t mesh = mem.readPtr(pawn + g_off.APawn_Mesh);
    if (!mesh) return;

    if (g_off.Mesh_LastRenderTime) {
        const float v = mem.read<float>(mesh + g_off.Mesh_LastRenderTime);
        static std::map<uintptr_t, float> s_lrt;
        float& g = s_lrt[pawn];
        if (std::isfinite(v) && v > 0.f && v < 1e7f) {
            // rises normally; a large DROP is the world clock restarting on a
            // round change, which must be accepted rather than ignored
            if (v > g || v < g - 5.0f) g = v;
        }
        ent.lastRenderTime = g;
        if (s_lrt.size() > 256) s_lrt.clear();
    }

    if (!g_off.Mesh_BoneArray || !g_off.Mesh_ComponentToWorld) return;
    uintptr_t data = mem.readPtr(mesh + g_off.Mesh_BoneArray);
    int32_t   num  = mem.read<int32_t>(mesh + g_off.Mesh_BoneArray + 8);
    if (!data || num < 8 || num > 512) return;

    // ComponentToWorld
    uintptr_t ctw = mesh + g_off.Mesh_ComponentToWorld;
    FQuat   cq = mem.read<FQuat>  (ctw + FTransformLayout::Rotation);
    FVector ct = mem.read<FVector>(ctw + FTransformLayout::Translation);
    FVector cs = mem.read<FVector>(ctw + FTransformLayout::Scale3D);
    double qn = std::sqrt(cq.X*cq.X + cq.Y*cq.Y + cq.Z*cq.Z + cq.W*cq.W);
    if (!(qn > 0.9 && qn < 1.1)) return;          // not a real transform

    // Head: the on-axis bone with the greatest local Z.
    int   bestIdx = -1;
    double bestZ  = 60.0;                          // must clear the torso
    for (int i = 0; i < num && i < 256; i++) {
        FVector t = mem.read<FVector>(data + (uintptr_t)i * FTransformLayout::Size
                                          + FTransformLayout::Translation);
        if (std::isnan(t.X) || std::isnan(t.Y) || std::isnan(t.Z)) continue;
        if (std::fabs(t.X) > 20.0 || std::fabs(t.Y) > 20.0) continue;   // off-axis
        if (t.Z > bestZ) { bestZ = t.Z; bestIdx = i; }
    }
    if (bestIdx < 0) return;

    FVector hb = mem.read<FVector>(data + (uintptr_t)bestIdx * FTransformLayout::Size
                                       + FTransformLayout::Translation);
    // rotate by the component quaternion: v + 2w(qxv) + 2(qx(qxv))
    double vx = hb.X * cs.X, vy = hb.Y * cs.Y, vz = hb.Z * cs.Z;
    double ux = cq.Y*vz - cq.Z*vy, uy = cq.Z*vx - cq.X*vz, uz = cq.X*vy - cq.Y*vx;
    double wx = cq.Y*uz - cq.Z*uy, wy = cq.Z*ux - cq.X*uz, wz = cq.X*uy - cq.Y*ux;
    ent.headWorld = FVector(vx + 2.0*(cq.W*ux + wx) + ct.X,
                            vy + 2.0*(cq.W*uy + wy) + ct.Y,
                            vz + 2.0*(cq.W*uz + wz) + ct.Z);
    ent.feetWorld = FVector(ct.X, ct.Y, ct.Z);     // mesh origin = feet
    ent.hasSkeleton = true;

    // ── the real hierarchy, if it resolves
    if (!g_espSkeleton) return;
    skel::Rig* rig = skel::rigFor(mem, mesh);
    if (!rig || !rig->ok) return;
    if (num != rig->count) {
        static uintptr_t warned = 0;
        if (warned != rig->skeleton) {
            warned = rig->skeleton;
            printf("[skel] bone array has %d entries but the table has %d -- "
                   "different index spaces, rig rejected\n", num, rig->count);
        }
        return;
    }

    std::vector<uint8_t> raw(size_t(num) * skel::kTransform);
    if (!mem.read_raw(data, raw.data(), raw.size())) return;

    // The head's LATERAL position. Height is the game's own eye height; the
    // bones supply only X and Y, which is the one thing eye height cannot give.
    if (ent.capsuleHalf > 20.f && ent.eyeHeight > 0.f) {
        // the eye, measured from the mesh origin -- which IS the feet, a capsule
        // half-height below the root
        const double eyeLocal = (double)ent.capsuleHalf + (double)ent.eyeHeight;
        const double latMax = 0.22 * (double)ent.capsuleHalf * 2.0;
        FVector hl;
        bool gotLateral = false;

        if (rig->head >= 0 && rig->head < num) {
            FVector lp; FQuat lr;
            if (skel::composeOneOriented(raw, num, rig->parents, rig->head,
                                         lp, lr)) {
                const double bodyH = 2.0 * (double)ent.capsuleHalf;
                const FVector local{ bodyH * (double)g_aimHeadFwd, 0.0,
                                     bodyH * (double)g_aimHeadUp };
                const FVector off = skel::qrot(lr, local);
                const FVector p2{ (lp.X + off.X) * cs.X, (lp.Y + off.Y) * cs.Y,
                                  (lp.Z + off.Z) * cs.Z };
                const FVector w2 = skel::qrot(cq, p2);
                ent.headJoint = FVector(w2.X + ct.X, w2.Y + ct.Y, w2.Z + ct.Z);
                ent.hasHeadJoint = true;
                gotLateral = true;
            }
        }
        if (!gotLateral && skel::headLateral(*rig, raw, num, eyeLocal, latMax, hl)) {
            const FVector scaled{ hl.X * cs.X, hl.Y * cs.Y, hl.Z * cs.Z };
            const FVector r2 = skel::qrot(cq, scaled);
            // X and Y from the bone, Z from the game. The height can never be
            // wrong and the lateral now follows the head.
            ent.headJoint = FVector(r2.X + ct.X, r2.Y + ct.Y,
                                    ent.origin.Z + (double)ent.eyeHeight);
            ent.hasHeadJoint = true;
        }
    }

    auto take = [&](int idx, FVector& dst) {
        return idx >= 0 && skel::boneWorld(mem, mesh, *rig, idx, raw, num,
                                           cq, ct, cs, dst);
    };
    FVector lf{}, rf{};
    const bool okHead = take(rig->head,   ent.boneHead);
    const bool okCh   = take(rig->chest,  ent.boneChest);
    const bool okPel  = take(rig->pelvis, ent.bonePelvis);
    take(rig->lFoot, lf); take(rig->rFoot, rf);
    ent.boneFeet = FVector((lf.X + rf.X) * 0.5, (lf.Y + rf.Y) * 0.5,
                           (lf.Z + rf.Z) * 0.5);
    (void)okCh;
    ent.hasRig = rig->ok && (okHead || okPel || num > 8);

    if ((g_espSkeleton || g_trigSkeleton) && ent.hasRig) {
        ent.rig = rig;
        ent.rigCount = std::min(num, 128);
        for (int i = 0; i < ent.rigCount; ++i)
            if (!skel::boneWorld(mem, mesh, *rig, i, raw, num, cq, ct, cs,
                                 ent.rigBones[i]))
                ent.rigBones[i] = FVector(0, 0, 0);
    }
}

// the chain died at `self=0x0`.
// smaps rather than maps, because smaps says how much of each mapping holds
// memory. Most of the game's writable address space is reserved and never
// touched, hundreds of GB after a long session, and a sweep reads that as
// zeros at the full cost of reading. A mapping with nothing resident and
// nothing swapped out has nothing in it to find.
inline std::vector<MemRegion> heapRegionsFor(const Mem& mem) {
    std::vector<MemRegion> out;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps", mem.pid);
    std::ifstream f(path);
    std::string line;
    uintptr_t b = 0, e = 0;
    bool writable = false;
    size_t kb = 0;
    auto flush = [&]() {
        if (e <= b || !writable || (e - b) < 0x10000 || kb == 0) return;
        if (b >= mem.modbase && e <= mem.modend) return;
        out.push_back({b, e - b});
    };
    while (std::getline(f, line)) {
        const size_t sp = line.find(' ');
        const std::string head = line.substr(0, sp);
        if (head.find('-') != std::string::npos && head.find(':') == std::string::npos) {
            flush();
            b = strtoull(head.c_str(), nullptr, 16);
            e = strtoull(head.c_str() + head.find('-') + 1, nullptr, 16);
            writable = sp != std::string::npos && line.size() > sp + 2 &&
                       line[sp + 1] == 'r' && line[sp + 2] == 'w';
            kb = 0;
        } else if (head == "Rss:" || head == "Swap:") {
            kb += strtoull(line.c_str() + head.size(), nullptr, 10);
        }
    }
    flush();
    return out;
}

// Found during the same startup scan as the controller.
inline uintptr_t g_gameState = 0;
inline uintptr_t g_cameraMgr = 0;

// Result of the single startup scan.
struct LocalRefs {
    uintptr_t controller = 0;
    uintptr_t gameState  = 0;
};

inline bool validateGameState(const Mem& mem, uintptr_t gs) {
    using namespace offsets;
    uintptr_t data = mem.readPtr(gs + g_off.AGameStateBase_PlayerArray + TArray_Data);
    int32_t   num  = mem.read<int32_t>(gs + g_off.AGameStateBase_PlayerArray + TArray_Num);
    if (!data || (data & 7) || num < 2 || num > 64) return false;

    uintptr_t common = 0;
    int checked = 0, agree = 0, withPawn = 0;
    for (int i = 0; i < num && checked < 12; i++) {
        uintptr_t ps = mem.readPtr(data + i * 8);
        if (!ps || (ps & 7)) continue;
        uintptr_t vt = mem.readPtr(ps);
        if (!mem.vtableInModule(vt)) continue;
        checked++;
        if (!common) { common = vt; agree = 1; }
        else if (vt == common) agree++;
        if (g_off.APlayerState_PawnPrivate) {
            uintptr_t pawn = mem.readPtr(ps + g_off.APlayerState_PawnPrivate);
            if (pawn && !(pawn & 7) && mem.vtableInModule(mem.readPtr(pawn))) withPawn++;
        }
    }
    if (checked < 2 || agree < (checked + 1) / 2) return false;
    if (g_off.APlayerState_PawnPrivate && withPawn == 0) return false;

    if (!g_off.VT_APlayerState && common > mem.modbase && common < mem.modend)
        g_off.VT_APlayerState = common - mem.modbase;     // derived, not assumed
    return true;
}

// Our pawn's anim state updater keeps the GameState it animates against, so
// the live GameState is two reads from the pawn: no sweep, and never a
// GameState that an earlier match left behind in the heap. `known` has already
// been validated, so finding it again costs nothing more than the reads.
inline uintptr_t gameStateOfPawn(const Mem& mem, uintptr_t pawn, uintptr_t known = 0) {
    if (!pawn || (pawn & 7) || !g_off.ADiscoveryCharacter_AnimSU || !g_off.AnimSU_GameState)
        return 0;
    const uintptr_t asu = mem.readPtr(pawn + g_off.ADiscoveryCharacter_AnimSU);
    if (!asu || (asu & 7) || !mem.vtableInModule(mem.readPtr(asu))) return 0;
    const uintptr_t gs = mem.readPtr(asu + g_off.AnimSU_GameState);
    if (!gs || (gs & 7) || !mem.vtableInModule(mem.readPtr(gs))) return 0;
    if (gs == known) return gs;
    return validateGameState(mem, gs) ? gs : 0;
}

inline bool playerListHas(const Mem& mem, uintptr_t gs, uintptr_t ps) {
    const uintptr_t data = mem.readPtr(gs + g_off.AGameStateBase_PlayerArray
                                          + offsets::TArray_Data);
    const int32_t num = mem.read<int32_t>(gs + g_off.AGameStateBase_PlayerArray
                                             + offsets::TArray_Num);
    if (!data || num <= 0 || num > 128) return false;
    std::vector<uintptr_t> v((size_t)num);
    if (!mem.read_raw(data, v.data(), v.size() * sizeof(uintptr_t))) return false;
    return std::find(v.begin(), v.end(), ps) != v.end();
}

inline uintptr_t pickGameStateByClock(const Mem& mem,
                                      const std::vector<uintptr_t>& cands) {
    if (cands.empty()) return 0;
    if (cands.size() == 1 && !g_off.AGameStateBase_WorldTime) return cands[0];

    std::vector<double> before(cands.size());
    for (size_t i = 0; i < cands.size(); i++)
        before[i] = g_off.AGameStateBase_WorldTime
                  ? mem.read<double>(cands[i] + g_off.AGameStateBase_WorldTime) : 0.0;

    const double WAIT = 1.5;
    struct timespec ts{ (time_t)WAIT, (long)((WAIT - (long)WAIT) * 1e9) };
    nanosleep(&ts, nullptr);

    for (size_t i = 0; i < cands.size(); i++) {
        if (!g_off.AGameStateBase_WorldTime) continue;
        double a = before[i];
        double b = mem.read<double>(cands[i] + g_off.AGameStateBase_WorldTime);
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        // A real match clock is well past zero by the time anyone runs this,
        // and it advances by roughly the wall time. Near-zero values drifting a
        // little look identical to a clock and were being accepted.
        if (a < 5.0 || a > 1e7) continue;                 // seconds of a match
        double d = b - a;
        if (d < WAIT * 0.25 || d > WAIT * 4.0 + 1.0) continue;
        if (!validateGameState(mem, cands[i])) continue;  // still a real roster?
        printf("[reader] GameState 0x%lx - match clock at +0x%lX now %.1fs "
               "(%zu candidate(s) rejected)\n",
               cands[i], (unsigned long)g_off.AGameStateBase_WorldTime, b,
               cands.size() - 1);
        return cands[i];
    }
    printf("[reader] %zu object(s) held a player list but none carried an "
           "advancing match clock - refusing to guess.\n", cands.size());
    return 0;
}

// The local controller from GObjects -- no heap sweep.
inline bool pitchLooksHuman(double p) {
    if (!std::isfinite(p)) return false;
    double a = std::fmod(p, 360.0);
    if (a < 0) a += 360.0;
    return a <= 91.0 || a >= 269.0;
}

inline bool rollIsLevel(double r) {
    if (!std::isfinite(r)) return false;
    double a = std::fmod(r, 360.0);
    if (a < 0) a += 360.0;
    return a <= 1.0 || a >= 359.0;
}

// The camera manager is the object holding our controller at PCOwner, but it
// is not the only object that does, and projecting through one of the others
// puts every box off screen while the sonar, which needs no camera, still
// works. The camera class settles it. Without a class match, a POV that
// describes a real view near our own pawn is the next best evidence, and an
// object with neither is not a camera.
enum class CamFit { None, View, Class };
inline CamFit cameraIsOurs(const Mem& mem, uintptr_t cam, uintptr_t pawn) {
    if (g_off.VT_APlayerCameraManager &&
        mem.readPtr(cam) == mem.modbase + g_off.VT_APlayerCameraManager)
        return CamFit::Class;
    if (!g_off.APlayerCameraManager_POVLoc) return CamFit::None;
    const uintptr_t pov = cam + g_off.APlayerCameraManager_POVLoc;
    const float    f = mem.read<float>(pov + povFovOff());
    const FRotator r = mem.read<FRotator>(pov + povRotOff());
    const FVector  L = mem.read<FVector>(pov);
    if (!(f > 1.f && f < 170.f)) return CamFit::None;
    if (!std::isfinite(r.Yaw) || std::fabs(r.Yaw) > 361.0) return CamFit::None;
    if (!pitchLooksHuman(r.Pitch) || !std::isfinite(r.Roll)) return CamFit::None;
    double roll = std::fmod(r.Roll, 360.0);
    if (roll < 0) roll += 360.0;
    if (roll > 5.0 && roll < 355.0) return CamFit::None;    // shake, not a tilt
    if (!std::isfinite(L.X) || !std::isfinite(L.Y) || !std::isfinite(L.Z) || L.isZero())
        return CamFit::None;
    uintptr_t root = pawn ? mem.readPtr(pawn + g_off.AActor_RootComponent) : 0;
    if (root && !(root & 7)) {
        const FVector me = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
        if (!me.isZero() && L.dist(me) > 5000.0) return CamFit::None;
    }
    return CamFit::View;
}

inline uintptr_t findLocalControllerViaGObjects(const Mem& mem) {
    if (!g_off.GObjects_RVA) return 0;
    GObjectsView g = godirect::resolve(mem);
    if (!g.ok) {
        printf("[gobjects] global at modbase+0x%lX did not decode -- "
               "falling back to the sweep\n", (unsigned long)g_off.GObjects_RVA);
        return 0;
    }
    if (!godirect::verify(mem, g)) {
        printf("[gobjects] decoded 0x%lx but index correspondence FAILED -- "
               "not trusting it\n", (unsigned long)g.base);
        return 0;
    }
    printf("[gobjects] 0x%lx  %d objects  (no heap sweep)\n",
           (unsigned long)g.base, g.count);

    auto looksReal = [&](uintptr_t ctrl, uintptr_t pawn) -> bool {
        // a controller owns a PlayerState
        if (g_off.AController_PlayerState) {
            uintptr_t ps = mem.readPtr(ctrl + g_off.AController_PlayerState);
            if (!ps || (ps & 7) || !mem.vtableInModule(mem.readPtr(ps))) return false;
        }
        // and a rotation a human could be looking along -- upright means roll 0
        if (g_off.AController_ControlRotation) {
            FRotator r = mem.read<FRotator>(ctrl + g_off.AController_ControlRotation);
            if (!std::isfinite(r.Pitch) || !std::isfinite(r.Yaw) || !std::isfinite(r.Roll))
                return false;
            if (!pitchLooksHuman(r.Pitch) || !rollIsLevel(r.Roll)) return false;
            if (!std::isfinite(r.Yaw) || std::fabs(r.Yaw) > 361.0) return false;
        }
        // the pawn stands somewhere in the world
        uintptr_t root = mem.readPtr(pawn + g_off.AActor_RootComponent);
        if (!root || (root & 7) || !mem.vtableInModule(mem.readPtr(root))) return false;
        FVector p = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
        if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z))
            return false;
        if (p.isZero() || std::fabs(p.X) > 1e7 || std::fabs(p.Y) > 1e7) return false;
        return true;
    };

    static std::vector<uintptr_t> objs;
    static std::chrono::steady_clock::time_point lastEnum{};
    static int lastCount = 0;
    auto nowT = std::chrono::steady_clock::now();
    bool stale = objs.empty() ||
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     nowT - lastEnum).count() > 3000 ||
                 std::abs(g.count - lastCount) > 2048;
    if (stale) {
        godirect::allObjects(mem, g, objs);
        lastEnum = nowT;
        lastCount = g.count;
        printf("[gobjects] %zu object pointers read in blocks\n", objs.size());
    }

    // Count what happens at every gate, so a run that finds nobody says which
    // test rejected everything rather than only that nothing survived.
    int nObj = 0, nVt = 0, nPawn = 0, nCycle = 0, nReal = 0;
    uintptr_t firstCycleCtrl = 0, firstCyclePawn = 0;

    for (size_t i = 0; i < objs.size(); i++) {
        uintptr_t o = objs[i];
        if (!o || (o & 7) || o < 0x10000) continue;
        nObj++;
        if (!mem.vtableInModule(mem.readPtr(o))) continue;
        nVt++;
        uintptr_t pawn = mem.readPtr(o + g_off.AController_Pawn);
        if (!pawn || (pawn & 7) || !mem.vtableInModule(mem.readPtr(pawn))) continue;
        nPawn++;
        if (mem.readPtr(pawn + g_off.APawn_Controller) != o) continue;
        nCycle++;
        if (!firstCycleCtrl) { firstCycleCtrl = o; firstCyclePawn = pawn; }
        if (!looksReal(o, pawn)) continue;   // a coincidence, not the player
        nReal++;
        printf("[gobjects] controller 0x%lx <-> pawn 0x%lx  (index %zu)\n",
               (unsigned long)o, (unsigned long)pawn, i);

        auto learnVt = [&](const char* name, uintptr_t obj, uintptr_t* slot) {
            if (!obj) return;
            uintptr_t vt = mem.readPtr(obj);
            if (vt <= mem.modbase || vt >= mem.modend) return;
            uintptr_t rva = vt - mem.modbase;
            if (*slot == rva) return;
            printf("[gobjects] %-24s = 0x%lX   (was 0x%lX)\n", name,
                   (unsigned long)rva, (unsigned long)*slot);
            *slot = rva;
        };
        learnVt("VT_APlayerController", o, &g_off.VT_APlayerController);
        learnVt("VT_APawn", pawn, &g_off.VT_APawn);
        if (g_off.AController_PlayerState)
            learnVt("VT_APlayerState",
                    mem.readPtr(o + g_off.AController_PlayerState),
                    &g_off.VT_APlayerState);

        if (g_off.APlayerCameraManager_PCOwner) {
            uintptr_t found = 0;
            for (size_t k = 0; k < objs.size(); k++) {
                uintptr_t c = objs[k];
                if (!c || (c & 7) || c < 0x10000) continue;
                if (!mem.vtableInModule(mem.readPtr(c))) continue;
                if (mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) != o) continue;
                const CamFit fit = cameraIsOurs(mem, c, pawn);
                if (fit == CamFit::Class) { found = c; break; }
                if (fit == CamFit::View && !found) found = c;
            }
            if (found) {
                if (found != g_cameraMgr) {
                    g_cameraMgr = found;
                    float f = mem.read<float>(found + g_off.APlayerCameraManager_POVLoc
                                              + povFovOff());
                    if (!(f > 1.f && f < 170.f)) {
                        uintptr_t root = mem.readPtr(pawn + g_off.AActor_RootComponent);
                        FVector me{};
                        if (root && !(root & 7))
                            me = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
                        for (uintptr_t x = 0x300; x < 0x900; x += 8) {
                            FVector  L = mem.read<FVector> (found + x);
                            FRotator R = mem.read<FRotator>(found + x + povRotOff());
                            float    F = mem.read<float>   (found + x + povFovOff());
                            if (!std::isfinite(L.X) || !std::isfinite(R.Yaw) ||
                                !std::isfinite(F)) continue;
                            if (!(F > 1.f && F < 170.f)) continue;
                            if (!rollIsLevel(R.Roll) || !pitchLooksHuman(R.Pitch))
                                continue;
                            if (std::fabs(R.Yaw) > 361.0) continue;
                            if (!me.isZero() && L.dist(me) > 3000.0) continue;
                            printf("[gobjects] POVLoc derived: camera+0x%lX "
                                   "(offsets.cfg had 0x%lX, FOV there was %.1f)\n",
                                   (unsigned long)x,
                                   (unsigned long)g_off.APlayerCameraManager_POVLoc, f);
                            g_off.APlayerCameraManager_POVLoc = x;
                            f = F;
                            break;
                        }
                    }
                    printf("[gobjects] camera manager 0x%lx via PCOwner  (FOV %.1f)%s\n",
                           (unsigned long)found, f,
                           (f > 1.f && f < 170.f) ? ""
                           : "   <-- no POV block found, FOV stays fixed");
                    learnVt("VT_APlayerCameraManager", found,
                            &g_off.VT_APlayerCameraManager);
                }
            } else if (!g_cameraMgr) {
                printf("[gobjects] no camera manager points back at this controller "
                       "-- FOV stays fixed at %.0f, so boxes will not scale when "
                       "you scope\n", (double)g_fov);
            }
        }

        if (g_off.ADiscoveryCharacter_AnimSU && g_off.AnimSU_GameState) {
            uintptr_t asu = mem.readPtr(pawn + g_off.ADiscoveryCharacter_AnimSU);
            if (asu && mem.vtableInModule(mem.readPtr(asu))) {
                uintptr_t gs = mem.readPtr(asu + g_off.AnimSU_GameState);
                if (gs && mem.vtableInModule(mem.readPtr(gs))) {
                    double clock = g_off.AGameStateBase_WorldTime
                                 ? mem.read<double>(gs + g_off.AGameStateBase_WorldTime) : 0.0;
                    int32_t num = mem.read<int32_t>(gs + g_off.AGameStateBase_PlayerArray
                                                       + offsets::TArray_Num);
                    const bool clockOk = !g_off.AGameStateBase_WorldTime
                                      || (clock > 1.0 && clock < 1e7);
                    if (clockOk && num >= 1 && num < 64) {
                        // Only announce a CHANGE -- this now runs on every
                        // re-resolve, and printing each time would bury the
                        // one line that matters: the match handover.
                        if (gs != g_gameState) {
                            printf("[gobjects] gamestate 0x%lx via AnimSU  "
                                   "(clock %.1fs, %d players)%s\n",
                                   (unsigned long)gs, clock, num,
                                   g_gameState ? "   <- NEW MATCH" : "");
                            g_gameState = gs;
                            learnVt("VT_AGameStateBase", gs,
                                    &g_off.VT_AGameStateBase);
                        }
                    } else if (gs != g_gameState) {
                        printf("[gobjects] AnimSU -> 0x%lx but clock %.1f / players %d "
                               "are not sane -- keeping 0x%lx\n",
                               (unsigned long)gs, clock, num,
                               (unsigned long)g_gameState);
                    }
                }
            }
        }

        if (!g_gameState) {
            for (size_t k = 0; k < objs.size(); k++) {
                uintptr_t c = objs[k];
                if (!c || (c & 7) || c < 0x10000) continue;
                if (!mem.vtableInModule(mem.readPtr(c))) continue;
                double clk = g_off.AGameStateBase_WorldTime
                           ? mem.read<double>(c + g_off.AGameStateBase_WorldTime) : 0.0;
                if (g_off.AGameStateBase_WorldTime && !(clk > 5.0 && clk < 1e7)) continue;
                if (!validateGameState(mem, c)) continue;
                g_gameState = c;
                int32_t n = mem.read<int32_t>(c + g_off.AGameStateBase_PlayerArray
                                                + offsets::TArray_Num);
                printf("[gobjects] gamestate 0x%lx found in the object array "
                       "(clock %.1fs, %d players) -- AnimSU was unavailable\n",
                       (unsigned long)c, clk, n);
                learnVt("VT_AGameStateBase", c, &g_off.VT_AGameStateBase);
                break;
            }
        }
        {
            {
                {
                }
            }
        }
        return o;
    }
    printf("[gobjects] no usable cycle. gates: %zu pointers -> %d aligned -> "
           "%d with vtable -> %d with a pawn -> %d closed cycles -> %d passed "
           "validation\n", objs.size(), nObj, nVt, nPawn, nCycle, nReal);
    if (firstCycleCtrl) {
        // A cycle existed and validation threw it away. Say WHICH test failed,
        // with the values, instead of reporting nothing.
        uintptr_t ps = mem.readPtr(firstCycleCtrl + g_off.AController_PlayerState);
        FRotator r = mem.read<FRotator>(firstCycleCtrl + g_off.AController_ControlRotation);
        uintptr_t root = mem.readPtr(firstCyclePawn + g_off.AActor_RootComponent);
        FVector pos{};
        if (root && !(root & 7)) pos = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
        printf("           first rejected cycle: ctrl 0x%lx pawn 0x%lx\n"
               "             PlayerState 0x%lx %s\n"
               "             ControlRotation %.2f %.2f %.2f %s\n"
               "             RootComponent 0x%lx  pos %.0f %.0f %.0f %s\n",
               (unsigned long)firstCycleCtrl, (unsigned long)firstCyclePawn,
               (unsigned long)ps,
               (ps && !(ps & 7) && mem.vtableInModule(mem.readPtr(ps))) ? "ok" : "<-- FAILED",
               r.Pitch, r.Yaw, r.Roll,
               (rollIsLevel(r.Roll) && pitchLooksHuman(r.Pitch)) ? "ok" : "<-- FAILED",
               (unsigned long)root, pos.X, pos.Y, pos.Z,
               (root && !pos.isZero()) ? "ok" : "<-- FAILED");
    }
    return 0;
}

inline uintptr_t findLocalController(const Mem& mem) {
    using namespace offsets;
    if (uintptr_t viaGO = findLocalControllerViaGObjects(mem))
        return viaGO;
    auto regions = heapRegionsFor(mem);
    const uintptr_t knownVt   = mem.modbase + g_off.VT_APlayerController;
    const uintptr_t knownGsVt = mem.modbase + g_off.VT_AGameStateBase;

    auto closesCycle = [&](uintptr_t x) -> bool {
        uintptr_t pawn = mem.readPtr(x + g_off.AController_Pawn);
        if (!pawn || (pawn & 15)) return false;
        return mem.readPtr(pawn + g_off.APawn_Controller) == x;
    };

    // A controller from the heap is trusted as ours when what it points at
    // agrees: its PlayerState names the same pawn, and that pawn reaches a
    // GameState whose player list holds the PlayerState. Objects an earlier
    // match left in the heap can still close the pawn cycle, so the first
    // controller the sweep meets is not necessarily live. One that agrees is,
    // and the sweep stops there instead of reading the rest of the heap.
    uintptr_t liveGs = 0;
    auto liveGameStateOf = [&](uintptr_t ctrl) -> uintptr_t {
        const uintptr_t pawn = mem.readPtr(ctrl + g_off.AController_Pawn);
        const uintptr_t gs = gameStateOfPawn(mem, pawn);
        if (!gs) return 0;
        if (!g_off.AController_PlayerState) return gs;
        const uintptr_t ps = mem.readPtr(ctrl + g_off.AController_PlayerState);
        if (!ps || mem.readPtr(ps + g_off.APlayerState_PawnPrivate) != pawn) return 0;
        return playerListHas(mem, gs, ps) ? gs : 0;
    };

    uintptr_t viaVtable = 0;
    std::vector<uintptr_t> viaCycle;
    std::vector<uintptr_t> camCandidates;
    std::vector<uintptr_t> gsCandidates;
    const uintptr_t knownCamVt = mem.modbase + g_off.VT_APlayerCameraManager;

    // One pass over the heap, in chunks, on every core. It stops as soon as it
    // holds a controller its own objects vouch for and, when the camera class
    // is known, the camera manager that points back at it: that is all startup
    // needs. Found one after the other on a single thread, the controller and
    // then the camera each cost a sweep of their own.
    struct Chunk { uintptr_t base; size_t size; };
    const size_t CH = 32u << 20;
    std::vector<Chunk> work;
    for (const MemRegion& r : regions)
        for (uintptr_t a = r.base; a < r.base + r.size; a += CH)
            work.push_back({a, std::min(CH, (size_t)(r.base + r.size - a))});
    const bool wantCam = g_off.VT_APlayerCameraManager && g_off.APlayerCameraManager_PCOwner;
    std::atomic<size_t> next{0};
    std::atomic<bool> done{false};
    std::atomic<bool> haveGs{false};         // a GameState is in hand
    std::mutex mu;
    uintptr_t liveCtrl = 0;                  // all of these under mu
    auto camFor = [&](uintptr_t ctrl) {
        for (uintptr_t c : camCandidates)
            if (mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) == ctrl) return true;
        return false;
    };
    unsigned nthreads = std::thread::hardware_concurrency();
    nthreads = std::max(2u, std::min(16u, nthreads));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; t++) {
        pool.emplace_back([&]() {
            std::vector<unsigned char> b(CH + 0x600);
            const size_t need = (g_off.AController_Pawn / 8) + 2;
            for (;;) {
                if (done.load(std::memory_order_relaxed)) return;
                const size_t k = next.fetch_add(1);
                if (k >= work.size()) return;
                const Chunk& ck = work[k];
                size_t got = 0;
                if (!mem.read_raw_partial(ck.base, b.data(), ck.size + 0x600, got) || got < 0x600) {
                    if (!mem.read_raw_partial(ck.base, b.data(), ck.size, got) || got < 0x600)
                        continue;
                }
                const uint64_t* q = reinterpret_cast<const uint64_t*>(b.data());
                const size_t nq = got / 8;
                for (size_t i = 0; i + need < nq; i += 2) {
                    if (i * 8 >= ck.size) break;
                    const uint64_t vt = q[i];
                    if (!mem.vtableInModule(vt)) continue;
                    const uintptr_t x = ck.base + i * 8;
                    if (wantCam && vt == knownCamVt) {
                        std::lock_guard<std::mutex> lk(mu);
                        if (camCandidates.size() < 64) camCandidates.push_back(x);
                        if (liveCtrl &&
                            mem.readPtr(x + g_off.APlayerCameraManager_PCOwner) == liveCtrl)
                            done = true;
                    }
                    if (vt == knownGsVt) {
                        std::lock_guard<std::mutex> lk(mu);
                        if (!g_gameState && validateGameState(mem, x)) {
                            g_gameState = x;   // same pass, no second scan
                            haveGs = true;
                        }
                    }
                    // No usable GameState vtable (blank build, or it moved in a
                    // patch)? Then collect candidates structurally in the same
                    // pass and let the match clock decide between them afterwards.
                    if (g_off.AGameStateBase_PlayerArray) {
                        size_t ds = g_off.AGameStateBase_PlayerArray / 8;
                        if (i + ds + 1 < nq) {
                            uint64_t d = q[i + ds];
                            int32_t  m = (int32_t)(q[i + ds + 1] & 0xFFFFFFFFu);
                            if (d && !(d & 7) && m >= 2 && m <= 64 &&
                                !haveGs.load(std::memory_order_relaxed)) {
                                // Checked outside the lock: plenty of objects
                                // have a small list at this offset, and holding
                                // it while each is read would put every thread
                                // back in single file.
                                bool wanted;
                                {
                                    std::lock_guard<std::mutex> lk(mu);
                                    wanted = !g_gameState && !liveGs && gsCandidates.size() < 64;
                                }
                                if (wanted && validateGameState(mem, x)) {
                                    std::lock_guard<std::mutex> lk(mu);
                                    if (gsCandidates.size() < 64) gsCandidates.push_back(x);
                                }
                            }
                        }
                    }
                    if (vt == knownVt) {
                        if (closesCycle(x)) {
                            const uintptr_t gs = liveGameStateOf(x);
                            std::lock_guard<std::mutex> lk(mu);
                            if (!liveCtrl) viaVtable = x;
                            if (gs && !liveCtrl) {
                                liveCtrl = x;
                                liveGs = gs;
                                viaVtable = x;
                                haveGs = true;
                                if (!wantCam || camFor(x)) done = true;
                            }
                        }
                        continue;
                    }
                    // fallback candidates: any object closing the cycle
                    uint64_t pawn = q[i + g_off.AController_Pawn / 8];
                    if (!pawn || (pawn & 15) || pawn == x) continue;
                    if (!mem.vtableInModule(mem.readPtr(pawn))) continue;
                    if (mem.readPtr(pawn + g_off.APawn_Controller) == x) {
                        std::lock_guard<std::mutex> lk(mu);
                        viaCycle.push_back(x);
                    }
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (liveGs) g_gameState = liveGs;
    std::vector<unsigned char> buf((32u << 20) + 0x600);

    if (!g_gameState && !gsCandidates.empty()) {
        g_gameState = pickGameStateByClock(mem, gsCandidates);
        if (g_gameState) {
            uintptr_t gvt = mem.readPtr(g_gameState);
            if (gvt > mem.modbase && gvt < mem.modend)
                g_off.VT_AGameStateBase = gvt - mem.modbase;
            printf("[reader] derived VT_AGameStateBase 0x%lX, VT_APlayerState 0x%lX\n",
                   (unsigned long)g_off.VT_AGameStateBase,
                   (unsigned long)g_off.VT_APlayerState);
        }
    }

    // The camera manager for this controller. The controller's own
    // PlayerCameraManager pointer is the direct way, since the engine spawns
    // the camera manager on clients too. Without that offset the camera is
    // matched by its PCOwner back-pointer instead, and because more than one
    // object holds the controller at that offset, a match has to be the
    // camera class or at least carry a real view near our pawn.
    auto pickCamera = [&](uintptr_t ctrl) {
        if (g_cameraMgr && g_off.APlayerCameraManager_PCOwner &&
            mem.readPtr(g_cameraMgr + g_off.APlayerCameraManager_PCOwner) != ctrl)
            g_cameraMgr = 0;                    // the last match's camera
        if (g_off.APlayerController_CameraManager) {
            const uintptr_t c = mem.readPtr(ctrl + g_off.APlayerController_CameraManager);
            if (c && !(c & 7) && mem.vtableInModule(mem.readPtr(c)) &&
                (!g_off.APlayerCameraManager_PCOwner ||
                 mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) == ctrl)) {
                if (c != g_cameraMgr)
                    printf("[reader] APlayerCameraManager = 0x%lx (the controller's own "
                           "pointer)\n", c);
                g_cameraMgr = c;
                return;
            }
        }
        if (g_cameraMgr || !g_off.APlayerCameraManager_PCOwner) return;
        const uintptr_t pawn = mem.readPtr(ctrl + g_off.AController_Pawn);
        for (uintptr_t c : camCandidates) {
            if (mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) != ctrl) continue;
            g_cameraMgr = c;
            printf("[reader] APlayerCameraManager = 0x%lx (camera class, PCOwner match)\n", c);
            return;
        }
        const size_t slot = g_off.APlayerCameraManager_PCOwner / 8;
        int notCamera = 0;
        for (const MemRegion& r : regions) {
            for (uintptr_t a = r.base; a < r.base + r.size; ) {
                size_t n = std::min((size_t)(32u << 20), (size_t)(r.base + r.size - a));
                size_t got = 0;
                if (!mem.read_raw_partial(a, buf.data(), n + 0x600, got) || got < 0x600) {
                    if (!mem.read_raw_partial(a, buf.data(), n, got) || got < 0x600) { a += n; continue; }
                }
                const uint64_t* q2 = reinterpret_cast<const uint64_t*>(buf.data());
                size_t nq2 = got / 8;
                for (size_t i = 0; i + slot < nq2; i += 2) {
                    if (i * 8 >= n) break;
                    if (q2[i + slot] != ctrl) continue;
                    if (!mem.vtableInModule(q2[i])) continue;   // a real object
                    uintptr_t x = a + i * 8;
                    const CamFit fit = cameraIsOurs(mem, x, pawn);
                    if (fit == CamFit::None) { notCamera++; continue; }
                    g_cameraMgr = x;
                    printf("[reader] APlayerCameraManager = 0x%lx (%s, found by PCOwner "
                           "back-pointer)\n", x,
                           fit == CamFit::Class ? "camera class" : "its POV is a real view");
                    return;
                }
                a += n;
            }
        }
        printf("[reader] WARNING: %d object(s) hold the controller at PCOwner and none "
               "is a camera - falling back to ControlRotation + BaseEyeHeight "
               "(set g_fov to your in-game FOV)\n", notCamera);
    };

    if (viaVtable) {
        printf("[reader] local APlayerController = 0x%lx (known vtable RVA 0x%X, cycle OK)\n",
               viaVtable, (unsigned)g_off.VT_APlayerController);
        pickCamera(viaVtable);
        return viaVtable;
    }
    if (viaCycle.size() == 1) {
        uintptr_t x = viaCycle[0];
        printf("[reader] local APlayerController = 0x%lx via cycle; vtable RVA is now 0x%lx\n"
               "         (moved - update g_off.VT_APlayerController)\n",
               x, mem.readPtr(x) - mem.modbase);
        pickCamera(x);
        return x;
    }
    if (viaCycle.empty()) {
        printf("[reader] no Controller<->Pawn cycle found - are you in a match?\n");
        return 0;
    }

    std::vector<uintptr_t> real;
    for (uintptr_t x : viaCycle) {
        uintptr_t pawn = mem.readPtr(x + g_off.AController_Pawn);
        if (g_off.AController_PlayerState) {
            uintptr_t ps = mem.readPtr(x + g_off.AController_PlayerState);
            if (!ps || (ps & 7) || !mem.vtableInModule(mem.readPtr(ps))) continue;
        }
        if (g_off.AController_ControlRotation) {
            FRotator r = mem.read<FRotator>(x + g_off.AController_ControlRotation);
            if (!std::isfinite(r.Yaw) || std::fabs(r.Yaw) > 361.0) continue;
            if (!pitchLooksHuman(r.Pitch) || !rollIsLevel(r.Roll)) continue;
        }
        uintptr_t root = pawn ? mem.readPtr(pawn + g_off.AActor_RootComponent) : 0;
        if (!root || (root & 7) || !mem.vtableInModule(mem.readPtr(root))) continue;
        FVector p = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
        if (!std::isfinite(p.X) || p.isZero() || std::fabs(p.X) > 1e7) continue;
        real.push_back(x);
    }
    printf("[reader] %zu cycle(s) found, %zu behave like a real player\n",
           viaCycle.size(), real.size());
    if (real.size() == 1 || (real.size() > 1 && viaCycle.size() > 1)) {
        uintptr_t x = real[0];
        printf("[reader] local APlayerController = 0x%lx via cycle + behaviour; "
               "vtable RVA is now 0x%lx\n", x, mem.readPtr(x) - mem.modbase);
        g_off.VT_APlayerController = mem.readPtr(x) - mem.modbase;
        uintptr_t pawn = mem.readPtr(x + g_off.AController_Pawn);
        if (pawn) g_off.VT_APawn = mem.readPtr(pawn) - mem.modbase;
        pickCamera(x);
        return x;
    }
    printf("[reader] none of the %zu cycles behaves like a player - "
           "not guessing.\n", viaCycle.size());
    return 0;
}

// Touches no window, no GL, no X11.
inline void dumpDiagnostics(const Mem& mem, uintptr_t uworld) {
    using namespace offsets;
    printf("\n================ DIAGNOSTIC DUMP ================\n");
    printf("module      0x%lx - 0x%lx\n", mem.modbase, mem.modend);
    printf(".rdata      0x%lx - 0x%lx\n", mem.rdataStart, mem.rdataEnd);
    printf("UWorld      0x%lx  vtRVA 0x%lx\n", uworld, mem.readPtr(uworld) - mem.modbase);
    printf("  PersistentLevel 0x%lx   NetDriver 0x%lx   Levels.Num %d\n",
           mem.readPtr(uworld + g_off.UWorld_PersistentLevel),
           mem.readPtr(uworld + g_off.UWorld_NetDriver),
           mem.read<int32_t>(uworld + g_off.UWorld_Levels + TArray_Num));

    printf("\n[scan] resolving local player + GameState...\n");
    uintptr_t ctrl = findLocalController(mem);
    if (!ctrl) { printf("FAIL: no local controller\n"); return; }
    uintptr_t pawn = mem.readPtr(ctrl + g_off.AController_Pawn);
    uintptr_t ps   = mem.readPtr(ctrl + g_off.AController_PlayerState);
    printf("Controller  0x%lx  vtRVA 0x%lx\n", ctrl, mem.readPtr(ctrl) - mem.modbase);
    printf("Pawn        0x%lx  vtRVA 0x%lx\n", pawn, pawn ? mem.readPtr(pawn) - mem.modbase : 0);
    printf("PlayerState 0x%lx\n", ps);

    // camera chain - the most likely cause of "everything projects off-screen"
    // Diagnostic only. Uses the DERIVED camera offset; there is no compiled
    // fallback, because a stale one would report "valid object" for the wrong
    // field and send you chasing a camera bug that does not exist.
    uintptr_t cam = 0;
    if (g_off.APlayerCameraManager_PCOwner) {
        cam = mem.readPtr(ctrl + g_off.APlayerCameraManager_PCOwner);
        printf("\n[camera] via PCOwner @ctrl+0x%X = 0x%lx  %s\n",
               (unsigned)g_off.APlayerCameraManager_PCOwner, cam,
               (cam && mem.vtableInModule(mem.readPtr(cam))) ? "(valid object)"
                                                            : "<-- INVALID");
    } else {
        printf("\n[camera] no derived camera offset -- run the update tool\n");
    }
    if (cam && mem.vtableInModule(mem.readPtr(cam))) {
        uintptr_t povLoc = cam + g_off.APlayerCameraManager_POVLoc;
        FVector  L = mem.read<FVector> (povLoc);
        FRotator R = mem.read<FRotator>(povLoc + povRotOff());
        float    F = mem.read<float>   (povLoc + povFovOff());
        printf("  POV loc  %.1f %.1f %.1f\n", L.X, L.Y, L.Z);
        printf("  POV rot  %.2f %.2f %.2f\n", R.Pitch, R.Yaw, R.Roll);
        printf("  POV FOV  %.2f  %s\n", F,
               (F > 1.f && F < 170.f) ? "(sane)" : "<-- INSANE, camera chain is wrong");
    }
    printf("  (compare: g_off.AController_ControlRotation = %.2f %.2f %.2f  <- known good)\n",
           mem.read<FRotator>(ctrl + g_off.AController_ControlRotation).Pitch,
           mem.read<FRotator>(ctrl + g_off.AController_ControlRotation).Yaw,
           mem.read<FRotator>(ctrl + g_off.AController_ControlRotation).Roll);

    // self position
    uintptr_t root = pawn ? mem.readPtr(pawn + g_off.AActor_RootComponent) : 0;
    FVector selfPos{};
    if (root) selfPos = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
    printf("\n[self] RootComponent 0x%lx  pos %.1f %.1f %.1f\n",
           root, selfPos.X, selfPos.Y, selfPos.Z);

    // entity list
    if (!g_gameState) { printf("\nFAIL: no GameState\n"); return; }
    uintptr_t arr = mem.readPtr(g_gameState + g_off.AGameStateBase_PlayerArray + TArray_Data);
    int32_t   num = mem.read<int32_t>(g_gameState + g_off.AGameStateBase_PlayerArray + TArray_Num);
    printf("\n[entities] GameState 0x%lx  PlayerArray Data=0x%lx Num=%d\n",
           g_gameState, arr, num);
    if (!arr || num <= 0 || num > 64) { printf("FAIL: bad PlayerArray\n"); return; }
    printf("  %-3s %-16s %-16s %-22s %9s %s\n", "#", "PlayerState", "Pawn", "position", "dist(m)", "name");
    for (int i = 0; i < num; i++) {
        uintptr_t e = mem.readPtr(arr + i * 8);
        if (!e) continue;
        uintptr_t p = mem.readPtr(e + g_off.APlayerState_PawnPrivate);
        std::string nm = readFString(mem, e + g_off.APlayerState_DisplayName);
        std::string tag = readFString(mem, e + g_off.APlayerState_Discriminator);
        FVector pos{};
        if (p) {
            uintptr_t rc = mem.readPtr(p + g_off.AActor_RootComponent);
            if (rc) pos = mem.read<FVector>(rc + g_off.USceneComponent_RelLocation);
        }
        double d = pos.dist(selfPos) / 100.0;
        printf("  %-3d 0x%-14lx 0x%-14lx %7.0f %7.0f %6.0f %8.1f  %s#%s\n",
               i, e, p, pos.X, pos.Y, pos.Z, d, nm.c_str(), tag.c_str());
    }
    printf("================================================\n\n");
}

// Main reader thread
inline bool worldStillValid(const Mem& mem, uintptr_t ctrl) {
    if (!ctrl || !mem.vtableInModule(mem.readPtr(ctrl))) return false;
    if (!g_gameState || !mem.vtableInModule(mem.readPtr(g_gameState))) return false;
    // PlayerArray must still look like a roster.
    int32_t num = mem.read<int32_t>(g_gameState + g_off.AGameStateBase_PlayerArray
                                    + offsets::TArray_Num);
    if (num < 1 || num > 128) return false;
    uintptr_t data = mem.readPtr(g_gameState + g_off.AGameStateBase_PlayerArray);
    if (!data || (data & 7)) return false;
    return true;
}

inline void readerThread(uintptr_t UWorld2f) {
    using namespace offsets;
    auto& mem = g_mem;

    (void)UWorld2f;

    // Preferred path: the engine's object array, which gives every live object
    // by index, so the player is found by reading rather than by searching. The
    // heap sweep is kept for when the array's constants are missing or do not
    // decode, which is what a game patch does to them.
    uintptr_t localCtrl = 0, localPawn = 0;
    if (!localCtrl) {
        printf("[reader] resolving the local player...\n");
        localCtrl = findLocalController(mem);
        localPawn = mem.readPtr(localCtrl + g_off.AController_Pawn);
    }
    if (!localCtrl)
        printf("[reader] no local player yet -- will keep looking (menu? loading?)\n");
    printf("[reader] Pawn = 0x%lx   PlayerState = 0x%lx\n",
           localPawn, mem.readPtr(localCtrl + g_off.AController_PlayerState));
    if (g_gameState)
        printf("[reader] GameState = 0x%lx   PlayerArray.Num = %d\n", g_gameState,
               mem.read<int32_t>(g_gameState + g_off.AGameStateBase_PlayerArray + TArray_Num));

    auto lastScan = std::chrono::steady_clock::now();

    while (g_running) {
        auto t0 = std::chrono::steady_clock::now();

        // Follow our own pawn to the GameState. A new match brings a new one,
        // and this switches on the first frame, instead of waiting for the old
        // one to stop validating and a sweep to find the next.
        if (localCtrl && mem.vtableInModule(mem.readPtr(localCtrl))) {
            const uintptr_t gs = gameStateOfPawn(
                mem, mem.readPtr(localCtrl + g_off.AController_Pawn), g_gameState);
            if (gs && gs != g_gameState) {
                printf("[reader] GameState 0x%lx, reached from our pawn%s\n", gs,
                       g_gameState ? " (a new match)" : "");
                g_gameState = gs;
            }
        }

        // Re-resolve when the cached objects die (match end, map change, or the
        // tool started at the menu). The scan walks the heap, so it is rate
        // limited to once every 2s rather than run every frame.
        if (!worldStillValid(mem, localCtrl)) {
            // The chain costs six reads, so it can be re-walked every frame --
            // no rate limit and nothing stale to carry across a match change.
            bool got = false;
            if (!got &&
                std::chrono::duration_cast<std::chrono::milliseconds>(t0 - lastScan).count() > 2000) {
                lastScan = t0;
                g_entityCount = 0;
                // The sweep only looks for a GameState while none is held, so
                // the last match's has to be let go or it is never replaced.
                if (g_gameState && !validateGameState(mem, g_gameState)) g_gameState = 0;
                uintptr_t found = findLocalController(mem);
                if (found) {
                    localCtrl = found;
                    localPawn = mem.readPtr(localCtrl + g_off.AController_Pawn);
                    printf("[reader] re-resolved by sweep: controller 0x%lx  gamestate 0x%lx\n",
                           localCtrl, g_gameState);
                }
            }
            if (!worldStillValid(mem, localCtrl)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;                       // nothing live to read yet
            }
        }

        // ── local character (changes on respawn)
        uintptr_t ackpawn = mem.readPtr(localCtrl + g_off.AController_Pawn);

        // ── camera snapshot
        {
            ViewInfo vi;
            if (g_off.AController_ControlRotation)
                vi.Rotation = mem.read<FRotator>(localCtrl + g_off.AController_ControlRotation);
            uintptr_t camPawn = ackpawn ? ackpawn : localPawn;
            if (camPawn) {
                uintptr_t rc = mem.readPtr(camPawn + g_off.AActor_RootComponent);
                if (rc) {
                    vi.Location = mem.read<FVector>(rc + g_off.USceneComponent_RelLocation);
                    float eye = g_off.APawn_BaseEyeHeight
                              ? mem.read<float>(camPawn + g_off.APawn_BaseEyeHeight) : 0.f;
                    if (eye > 0.f && eye < 400.f) vi.Location.Z += eye;
                }
            }
            vi.FOV = g_fov;
            if (vi.FOV < 1.f || vi.FOV > 170.f) vi.FOV = 90.f;

            // Prefer the real POV, resolved by PCOwner during the startup scan.
            // The controller's own pointer is read every frame, so a camera
            // manager replaced during the session is picked up at once.
            if (g_off.APlayerController_CameraManager) {
                const uintptr_t c = mem.readPtr(localCtrl + g_off.APlayerController_CameraManager);
                if (c && c != g_cameraMgr && !(c & 7) && mem.vtableInModule(mem.readPtr(c)))
                    g_cameraMgr = c;
            }
            uintptr_t camMgr = g_off.APlayerCameraManager_POVLoc ? g_cameraMgr : 0;
            if (camMgr && mem.vtableInModule(mem.readPtr(camMgr))) {
                uintptr_t povLoc = camMgr + g_off.APlayerCameraManager_POVLoc;
                float f = mem.read<float>(povLoc + povFovOff());
                if (f > 1.f && f < 170.f) {
                    vi.Location = mem.read<FVector> (povLoc);
                    vi.Rotation = mem.read<FRotator>(povLoc + povRotOff());
                    vi.FOV      = f;
                }
            }

            {
                static const bool cDbg = getenv("ESP_DEBUG") != nullptr;
                static int cN = 0;
                if (cDbg && (cN++ % 120) == 0) {
                    FRotator ctrlRot = mem.read<FRotator>(
                        localCtrl + g_off.AController_ControlRotation);
                    printf("[cam] %s  loc %.0f %.0f %.0f  rot %.1f %.1f %.1f  fov %.1f"
                           "   | ControlRotation %.1f %.1f %.1f\n",
                           g_cameraMgr ? "POV" : "derived",
                           vi.Location.X, vi.Location.Y, vi.Location.Z,
                           vi.Rotation.Pitch, vi.Rotation.Yaw, vi.Rotation.Roll,
                           vi.FOV,
                           ctrlRot.Pitch, ctrlRot.Yaw, ctrlRot.Roll);
                }
            }

            std::lock_guard<std::mutex> lk(g_camMtx);
            g_camView = vi;
        }

        // ── local squad index
        // AEmbarkPlayerStateBase::Squad is the squad actor itself, one object
        // shared by squadmates, so comparing it needs no index to be right.
        // The squad component on the pawn is the way when that is missing.
        const bool squadByState = g_off.APlayerState_Squad != 0;
        const bool squads = g_off.ADiscoveryCharacter_Squad && g_off.Squad_Index;
        int localteam = -1;
        uintptr_t localSquad = 0;
        if (squadByState && g_off.AController_PlayerState) {
            const uintptr_t lps = mem.readPtr(localCtrl + g_off.AController_PlayerState);
            if (lps) localSquad = mem.readPtr(lps + g_off.APlayerState_Squad);
        } else if (ackpawn && squads) {
            uintptr_t sc = mem.readPtr(ackpawn + g_off.ADiscoveryCharacter_Squad);
            if (sc) localteam = mem.read<int32_t>(sc + g_off.Squad_Index);
        }

        // ── GameState → PlayerArray
        uintptr_t gameState    = g_gameState;
        uintptr_t playerArrPtr = mem.readPtr(gameState + g_off.AGameStateBase_PlayerArray
        + TArray_Data);
        int32_t   playerCount  = mem.read<int32_t>(gameState + g_off.AGameStateBase_PlayerArray
        + TArray_Num);

        if (!playerArrPtr || playerCount <= 0 || playerCount > kMaxEntities) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ── self position for distance
        FVector selfPos;
        if (ackpawn) {
            uintptr_t root = mem.readPtr(ackpawn + g_off.AActor_RootComponent);
            if (root) selfPos = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
        }

        // ── read all player states in one batch
        std::vector<uintptr_t> psArr(playerCount);
        mem.read_raw(playerArrPtr, psArr.data(), playerCount * sizeof(uintptr_t));

        // Squads are numbered in the order they are first seen, and the
        // numbering lasts as long as the GameState does, so a squad keeps its
        // colour while players join, leave and die.
        static std::map<uintptr_t, int> s_squadNo;
        static uintptr_t s_squadGs = 0;
        if (s_squadGs != gameState) { s_squadNo.clear(); s_squadGs = gameState; }
        if (squadByState && !localSquad && ackpawn) {
            for (int i = 0; i < playerCount; i++)
                if (psArr[i] && mem.readPtr(psArr[i] + g_off.APlayerState_PawnPrivate) == ackpawn) {
                    localSquad = mem.readPtr(psArr[i] + g_off.APlayerState_Squad);
                    break;
                }
        }

        // The spectator flag hides whoever has it set, so a byte that is not
        // the flag hides players, and a byte every player shares hides all of
        // them. A match cannot have every player with a pawn spectating, and
        // we are not spectating while we play our own pawn. When the byte says
        // either, it is not the flag, and it hides nobody until it stops.
        bool hideSpectators = false;
        if (g_off.APlayerState_Spectator) {
            int withPawn = 0, flagged = 0;
            bool selfFlagged = false;
            for (int i = 0; i < playerCount; i++) {
                const uintptr_t ps = psArr[i];
                if (!ps) continue;
                const uintptr_t pw = mem.readPtr(ps + g_off.APlayerState_PawnPrivate);
                if (!pw) continue;
                withPawn++;
                if (mem.read<uint8_t>(ps + g_off.APlayerState_Spectator) & (1 << 1)) {
                    flagged++;
                    if (pw == ackpawn) selfFlagged = true;
                }
            }
            hideSpectators = !selfFlagged && !(withPawn >= 2 && flagged == withPawn);
            static int s_hide = -1;
            if (s_hide != (int)hideSpectators && (s_hide != -1 || !hideSpectators)) {
                if (hideSpectators)
                    printf("[ents] APlayerState_Spectator 0x%lX fits the match again; "
                           "hiding spectators\n",
                           (unsigned long)g_off.APlayerState_Spectator);
                else
                    printf("[ents] APlayerState_Spectator 0x%lX marks %s as spectating, "
                           "which a match cannot have, so it is not the flag and "
                           "hides nobody\n", (unsigned long)g_off.APlayerState_Spectator,
                           selfFlagged ? "you while you play" : "every player with a pawn");
            }
            s_hide = (int)hideSpectators;
        }

        EntityData tempEnts[kMaxEntities] = {};
        int count = 0;
        // Why does a roster of 12 become one box? Six `continue`s can drop a
        // player and none of them said so. Count each: the answer is which
        // number is 11, not a guess about which offset moved.
        int dropNoPs = 0, dropNoPawn = 0, dropNoRoot = 0,
            dropZeroPos = 0, dropDist = 0, dropDead = 0;
        bool noHealth[kMaxEntities] = {};
        int healthRead = 0;

        for (int i = 0; i < playerCount && count < kMaxEntities; i++) {
            uintptr_t ps = psArr[i];
            if (!ps) { dropNoPs++; continue; }

            bool spectator = false;
            if (hideSpectators) {
                uint8_t flags = mem.read<uint8_t>(ps + g_off.APlayerState_Spectator);
                spectator = (flags & (1 << 1)) != 0;
            }

            uintptr_t pawn = mem.readPtr(ps + g_off.APlayerState_PawnPrivate);
            if (!pawn) { dropNoPawn++; continue; }

            EntityData& ent = tempEnts[count];
            ent.id     = ps;
            ent.isSelf = (pawn == ackpawn);
            ent.isSpectator = spectator;

            uintptr_t root = mem.readPtr(pawn + g_off.AActor_RootComponent);
            if (!root) { dropNoRoot++; continue; }
            ent.origin = mem.read<FVector>(root + g_off.USceneComponent_RelLocation);
            if (ent.origin.isZero()) { dropZeroPos++; continue; }

            bool gotVel = false;
            if (g_off.SceneComp_ComponentVelocity) {
                const FVector v = mem.read<FVector>(
                    root + g_off.SceneComp_ComponentVelocity);
                if (std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z)
                    && (v.X || v.Y || v.Z)
                    && std::fabs(v.X) < 1e5 && std::fabs(v.Y) < 1e5
                    && std::fabs(v.Z) < 1e5) {
                    ent.velocity = v; gotVel = true;
                }
            }

            // measured fallback: successive origins over successive samples,
            // keyed by PAWN so it survives the entity list being reordered.
            if (!gotVel) {
                struct Track { FVector pos; double t; FVector vel; };
                static std::map<uintptr_t, Track> s_track;
                const double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto it = s_track.find(pawn);
                if (it == s_track.end()) {
                    s_track[pawn] = Track{ent.origin, now, FVector(0,0,0)};
                } else {
                    const double dt = now - it->second.t;
                    if (dt > 0.002 && dt < 0.5) {
                        FVector raw((ent.origin.X - it->second.pos.X) / dt,
                                    (ent.origin.Y - it->second.pos.Y) / dt,
                                    (ent.origin.Z - it->second.pos.Z) / dt);
                        const double a = 0.35;          // EMA on the velocity
                        it->second.vel.X += (raw.X - it->second.vel.X) * a;
                        it->second.vel.Y += (raw.Y - it->second.vel.Y) * a;
                        it->second.vel.Z += (raw.Z - it->second.vel.Z) * a;
                        it->second.pos = ent.origin; it->second.t = now;
                    } else if (dt >= 0.5) {             // stale: restart cleanly
                        it->second.pos = ent.origin; it->second.t = now;
                        it->second.vel = FVector(0,0,0);
                    }
                    ent.velocity = it->second.vel;
                }
                if (s_track.size() > 256) s_track.clear();
            }

            ent.distance = (float)(ent.origin.dist(selfPos) / 100.0);
            if (ent.distance > g_maxEspDist) { dropDist++; continue; }

            // Health: two adjacent floats, the smaller of them the current one
            uintptr_t hc = (g_off.ADiscoveryCharacter_Health && g_off.Health_A && g_off.Health_B)
                         ? mem.readPtr(pawn + g_off.ADiscoveryCharacter_Health) : 0;
            bool hasHealth = false;
            if (hc) {
                float a = mem.read<float>(hc + g_off.Health_A);
                float b = mem.read<float>(hc + g_off.Health_B);
                if (std::isfinite(a) && std::isfinite(b) &&
                    a >= 0.f && b >= 0.f && a <= 1000.f && b <= 1000.f) {
                    hasHealth = true;
                    healthRead++;
                    ent.health    = (a < b) ? a : b;     // current is the smaller
                    ent.maxHealth = (a < b) ? b : a;
                    if (ent.maxHealth <= 0.0) ent.maxHealth = 100.0;
                    if (ent.health <= 0.0) { dropDead++; continue; }
                }
            }
            // The slot may still hold a player dropped just before, whose
            // numbers would stand in for the ones that could not be read.
            if (!hasHealth) { ent.health = 0.0; ent.maxHealth = 100.0; }
            noHealth[count] = !hasHealth;

            // Squad. Who is a squadmate comes from the squad actor, which needs no
            // number to be right. The colour needs the game's own number for the
            // squad, its place in the game state's squad list, which the pawn's
            // squad component holds: the palette follows that order, so a squad
            // is drawn in the colour the game gives it. Numbering squads in the
            // order they are first met is only the fallback, and it is arbitrary.
            if (squadByState) {
                const uintptr_t sq = mem.readPtr(ps + g_off.APlayerState_Squad);
                int idx = -1;
                if (squads) {
                    const uintptr_t sc = mem.readPtr(pawn + g_off.ADiscoveryCharacter_Squad);
                    if (sc && !(sc & 7)) idx = mem.read<int32_t>(sc + g_off.Squad_Index);
                    if (idx < 0 || idx > 63) idx = -1;
                }
                if (idx < 0 && sq) {
                    auto it = s_squadNo.find(sq);
                    if (it == s_squadNo.end() && s_squadNo.size() < 64)
                        it = s_squadNo.emplace(sq, (int)s_squadNo.size()).first;
                    if (it != s_squadNo.end()) idx = it->second;
                }
                ent.squadIdx = idx;
                ent.isTeammate = (localSquad && sq == localSquad && !ent.isSelf);
            } else {
                uintptr_t sc = squads ? mem.readPtr(pawn + g_off.ADiscoveryCharacter_Squad) : 0;
                if (sc) ent.squadIdx = mem.read<int32_t>(sc + g_off.Squad_Index);
                ent.isTeammate = (localteam >= 0 && ent.squadIdx == localteam && !ent.isSelf);
            }

            // Name. The engine's own PlayerNamePrivate is unused on this game;
            // what the match shows is DisplayName with the discriminator after
            // it, the same pair the scoreboard prints.
            if (g_off.APlayerState_DisplayName) {
                ent.name = readFString(mem, ps + g_off.APlayerState_DisplayName);
                if (g_off.APlayerState_Discriminator) {
                    std::string tag = readFString(mem, ps + g_off.APlayerState_Discriminator);
                    if (!tag.empty()) ent.name += "#" + tag;
                }
            }

            // Bones
            readBones(mem, pawn, ent);

            ent.valid = true;
            count++;
        }

        // A pawn with no health on it is a spectator's: the game never draws
        // it, nothing can hit it, and its box reads 0 health. The dead are gone
        // already, their health read as zero. When nobody's health could be
        // read at all, the reads are what failed, not the players, and nobody
        // is taken for a spectator.
        int specs = 0;
        for (int k = 0; k < count; k++) {
            if (healthRead > 0 && noHealth[k]) tempEnts[k].isSpectator = true;
            if (tempEnts[k].isSpectator && !tempEnts[k].isSelf) specs++;
        }

        // An empty screen has to say why without a debug build. While players
        // are listed and none is left to draw, print what removed them, again
        // every half minute while it lasts, and say when there is someone.
        // Squadmates are counted too: a squad offset that lands on something
        // every pawn has alike makes the whole lobby one squad, and this line
        // is where that shows.
        {
            int mates = 0, others = 0, selves = 0;
            for (int k = 0; k < count; k++) {
                if (tempEnts[k].isSelf) { selves++; continue; }
                if (tempEnts[k].isSpectator) continue;
                others++;
                if (tempEnts[k].isTeammate) mates++;
            }
            const int left = others - mates;
            const bool empty = playerCount >= 2 && left == 0;
            static bool s_empty = false, s_told = false;
            static std::chrono::steady_clock::time_point s_since{}, s_said{};
            if (empty && !s_empty) s_since = t0;
            if (empty && t0 - s_since > std::chrono::seconds(2) &&
                (!s_told || t0 - s_said > std::chrono::seconds(30))) {
                printf("[ents] %d in the player list, none to draw. dropped: no pawn "
                       "%d, no root %d, at origin %d, past max_esp_dist %d, dead %d, "
                       "empty slot %d. kept but not drawn: squadmate %d, spectator %d, "
                       "you %d\n", playerCount, dropNoPawn, dropNoRoot, dropZeroPos,
                       dropDist, dropDead, dropNoPs, mates, specs, selves);
                s_said = t0;
                s_told = true;
            }
            if (!empty && s_told) {
                printf("[ents] %d player(s) to draw again\n", left);
                s_told = false;
            }
            s_empty = empty;
        }

        {
            static const bool eDbg = getenv("ESP_DEBUG") != nullptr;
            static int dbgN = 0;
            if (eDbg && (dbgN++ % 120) == 0)
                printf("[ents] roster %d -> kept %d (spectators %d)   dropped: noPS %d  "
                       "noPawn %d  noRoot %d  zeroPos %d  tooFar %d  dead %d\n",
                       playerCount, count, specs, dropNoPs, dropNoPawn,
                       dropNoRoot, dropZeroPos, dropDist, dropDead);
        }

        // ── visibility verdict
        if (g_off.Mesh_LastRenderTime && count > 0) {
            const double wall =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const float estPrev = (g_visMaxSeen > 0.f)
                ? g_visMaxSeen + (float)(wall - g_visMaxWall) : 0.f;
            float mx = 0.f; int haveVals = 0;
            for (int k = 0; k < count; k++) {
                const float v = tempEnts[k].lastRenderTime;
                if (v <= 0.f) continue;
                if (estPrev > 0.f && v > estPrev + 2.0f) continue;   // impossible
                haveVals++; if (v > mx) mx = v;
            }

            bool usable = (haveVals > 0);

            if (usable) {
                if (mx > g_visMaxSeen || mx < g_visMaxSeen - 5.0f) {
                    g_visMaxSeen = mx; g_visMaxWall = wall;
                }
                const float est = g_visMaxSeen + (float)(wall - g_visMaxWall);
                {
                    int vis = 0, hid = 0;
                    for (int k = 0; k < count; k++) {
                        EntityData& e = tempEnts[k];
                        e.visible = (e.lastRenderTime > 0.f)
                                 && ((est - e.lastRenderTime) <= g_visTolerance);
                        if (e.visible) vis++; else hid++;
                    }
                    g_visVisibleCnt = vis; g_visHiddenCnt = hid;
                }
            }

            if (!usable) {
                for (int k = 0; k < count; k++) tempEnts[k].visible = true;
                g_visVisibleCnt = count; g_visHiddenCnt = 0;
            }
            g_visHave = usable;
        }

        {
            std::lock_guard<std::mutex> lk(g_entityMtx);
            for (int k = 0; k < count; k++) g_entities[k] = tempEnts[k];
            g_entityCount = count;
            g_entityGen++;
        }

        // ~60 Hz
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto sleep   = std::chrono::milliseconds(16) - elapsed;
        if (sleep.count() > 0)
            std::this_thread::sleep_for(sleep);
    }

    printf("[reader] thread done\n");
}
