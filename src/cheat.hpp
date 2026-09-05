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
#include <thread>
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
    uintptr_t cap = mem.readPtr(pawn + g_off.ACharacter_CapsuleComponent);
    if (cap) {
        float hh = mem.read<float>(cap + g_off.Capsule_HalfHeight);
        if (hh > 20.f && hh < 300.f) ent.capsuleHalf = hh;   // sane range only
    }

    // The game's own eye height for this pawn. Reflected, replicated, and exact.
    {
        const float eh = mem.read<float>(pawn + g_off.APawn_BaseEyeHeight);
        if (std::isfinite(eh) && eh > 10.f && eh < 200.f) ent.eyeHeight = eh;
    }

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
inline std::vector<MemRegion> heapRegionsFor(const Mem& mem) {
    std::vector<MemRegion> out;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", mem.pid);
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("rw") == std::string::npos) continue;
        uintptr_t b = strtoull(line.c_str(), nullptr, 16);
        auto dash = line.find('-');
        uintptr_t e = strtoull(line.c_str() + dash + 1, nullptr, 16);
        if (e <= b || (e - b) < 0x10000) continue;
        if (b >= mem.modbase && e <= mem.modend) continue;
        out.push_back({b, e - b});
    }
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
        uintptr_t ps = mem.readPtr(ctrl + g_off.AController_PlayerState);
        if (!ps || (ps & 7) || !mem.vtableInModule(mem.readPtr(ps))) return false;
        // and a rotation a human could be looking along -- upright means roll 0
        FRotator r = mem.read<FRotator>(ctrl + g_off.AController_ControlRotation);
        if (!std::isfinite(r.Pitch) || !std::isfinite(r.Yaw) || !std::isfinite(r.Roll))
            return false;
        if (!pitchLooksHuman(r.Pitch) || !rollIsLevel(r.Roll)) return false;
        if (!std::isfinite(r.Yaw) || std::fabs(r.Yaw) > 361.0) return false;
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

    // Count what happens at every gate. A validation added without a way to
    // see what it rejects is how "no cycle found" turned into a mystery twice.
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
                found = c;
                break;
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
                    double clock = mem.read<double>(gs + g_off.AGameStateBase_WorldTime);
                    int32_t num = mem.read<int32_t>(gs + g_off.AGameStateBase_PlayerArray
                                                       + offsets::TArray_Num);
                    if (clock > 1.0 && clock < 1e7 && num >= 1 && num < 64) {
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
                double clk = mem.read<double>(c + g_off.AGameStateBase_WorldTime);
                if (!(clk > 5.0 && clk < 1e7)) continue;
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

    std::vector<unsigned char> buf((32u << 20) + 0x600);
    uintptr_t viaVtable = 0;
    std::vector<uintptr_t> viaCycle;
    std::vector<uintptr_t> camCandidates;
    std::vector<uintptr_t> gsCandidates;
    const uintptr_t knownCamVt = mem.modbase + g_off.VT_APlayerCameraManager;

    for (const MemRegion& r : regions) {
        for (uintptr_t a = r.base; a < r.base + r.size; ) {
            size_t n = std::min((size_t)(32u << 20), (size_t)(r.base + r.size - a));
            size_t got = 0;
            if (!mem.read_raw_partial(a, buf.data(), n + 0x600, got) || got < 0x600) {
                if (!mem.read_raw_partial(a, buf.data(), n, got) || got < 0x600) { a += n; continue; }
            }
            const uint64_t* q = reinterpret_cast<const uint64_t*>(buf.data());
            size_t nq = got / 8;
            size_t need = (g_off.AController_Pawn / 8) + 2;
            for (size_t i = 0; i + need < nq; i += 2) {
                if (i * 8 >= n) break;
                uint64_t vt = q[i];
                if (!mem.vtableInModule(vt)) continue;
                uintptr_t x = a + i * 8;
                if (vt == knownGsVt && !g_gameState && validateGameState(mem, x)) {
                    g_gameState = x;   // same pass, no second scan
                }
                // No usable GameState vtable (blank build, or it moved in a
                // patch)? Then collect candidates structurally in the same
                // sweep and let the match clock decide between them afterwards.
                if (!g_gameState && g_off.AGameStateBase_PlayerArray) {
                    size_t ds = g_off.AGameStateBase_PlayerArray / 8;
                    if (i + ds + 1 < nq) {
                        uint64_t d = q[i + ds];
                        int32_t  m = (int32_t)(q[i + ds + 1] & 0xFFFFFFFFu);
                        if (d && !(d & 7) && m >= 2 && m <= 64 &&
                            gsCandidates.size() < 64 && validateGameState(mem, x))
                            gsCandidates.push_back(x);
                    }
                }
                if (vt == knownVt) { if (closesCycle(x)) viaVtable = x; continue; }
                // fallback candidates: any object closing the cycle
                uint64_t pawn = q[i + g_off.AController_Pawn / 8];
                if (!pawn || (pawn & 15) || pawn == x) continue;
                if (!mem.vtableInModule(mem.readPtr(pawn))) continue;
                if (mem.readPtr(pawn + g_off.APawn_Controller) == x) viaCycle.push_back(x);
            }
            a += n;
        }
        if (viaVtable && (g_gameState || !gsCandidates.empty()) && !camCandidates.empty()) break;
    }

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

    if (viaVtable) {
        printf("[reader] local APlayerController = 0x%lx (known vtable RVA 0x%X, cycle OK)\n",
               viaVtable, (unsigned)g_off.VT_APlayerController);
        // The controller does NOT store a usable PlayerCameraManager pointer
        // (0x488 reads NULL), so match the camera by its PCOwner back-pointer.
        for (uintptr_t c : camCandidates) {
            if (mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) == viaVtable) {
                g_cameraMgr = c;
                printf("[reader] APlayerCameraManager = 0x%lx (PCOwner match)\n", c);
                break;
            }
        }
        // The vtable RVA moves on every patch, so candidates gathered by vtable
        // alone vanish after an update. The back-pointer does not: exactly one
        // object in the heap holds this controller at +PCOwner. Sweep for that
        // directly, which needs no vtable at all.
        if (!g_cameraMgr && g_off.APlayerCameraManager_PCOwner) {
            const size_t slot = g_off.APlayerCameraManager_PCOwner / 8;
            for (const MemRegion& r : regions) {
                if (g_cameraMgr) break;
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
                        if (q2[i + slot] != viaVtable) continue;
                        uintptr_t x = a + i * 8;
                        if (!mem.vtableInModule(q2[i])) continue;   // a real object
                        g_cameraMgr = x;
                        printf("[reader] APlayerCameraManager = 0x%lx (found by PCOwner "
                               "back-pointer; vtable RVA is 0x%lx)\n",
                               x, (unsigned long)(q2[i] - mem.modbase));
                        break;
                    }
                    if (g_cameraMgr) break;
                    a += n;
                }
            }
        }
        if (!g_cameraMgr)
            printf("[reader] WARNING: no camera manager found - falling back to "
                   "ControlRotation + BaseEyeHeight (set g_fov to your in-game FOV)\n");
        return viaVtable;
    }
    if (viaCycle.size() == 1) {
        uintptr_t x = viaCycle[0];
        printf("[reader] local APlayerController = 0x%lx via cycle; vtable RVA is now 0x%lx\n"
               "         (moved - update g_off.VT_APlayerController)\n",
               x, mem.readPtr(x) - mem.modbase);
        return x;
    }
    if (viaCycle.empty()) {
        printf("[reader] no Controller<->Pawn cycle found - are you in a match?\n");
        return 0;
    }

    std::vector<uintptr_t> real;
    for (uintptr_t x : viaCycle) {
        uintptr_t pawn = mem.readPtr(x + g_off.AController_Pawn);
        uintptr_t ps   = mem.readPtr(x + g_off.AController_PlayerState);
        if (!ps || (ps & 7) || !mem.vtableInModule(mem.readPtr(ps))) continue;
        FRotator r = mem.read<FRotator>(x + g_off.AController_ControlRotation);
        if (!std::isfinite(r.Yaw) || std::fabs(r.Yaw) > 361.0) continue;
        if (!pitchLooksHuman(r.Pitch) || !rollIsLevel(r.Roll)) continue;
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
        for (uintptr_t c : camCandidates)
            if (mem.readPtr(c + g_off.APlayerCameraManager_PCOwner) == x) {
                g_cameraMgr = c;
                printf("[reader] APlayerCameraManager = 0x%lx (PCOwner match)\n", c);
                break;
            }
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

    // Preferred path: walk GEngine -> GameInstance -> LocalPlayer -> Controller.
    // Six pointer reads, no heap sweep, and correct again the instant a new
    // match creates new objects. The sweep is kept only for when the chain is
    // unavailable (GEngine_RVA not yet found) or returns nothing.
    uintptr_t localCtrl = 0, localPawn = 0;
    if (!localCtrl) {
        printf("[reader] scanning for the local player (Controller<->Pawn cycle)...\n");
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
            vi.Rotation = mem.read<FRotator>(localCtrl + g_off.AController_ControlRotation);
            uintptr_t camPawn = ackpawn ? ackpawn : localPawn;
            if (camPawn) {
                uintptr_t rc = mem.readPtr(camPawn + g_off.AActor_RootComponent);
                if (rc) {
                    vi.Location = mem.read<FVector>(rc + g_off.USceneComponent_RelLocation);
                    float eye = mem.read<float>(camPawn + g_off.APawn_BaseEyeHeight);
                    if (eye > 0.f && eye < 400.f) vi.Location.Z += eye;
                }
            }
            vi.FOV = g_fov;
            if (vi.FOV < 1.f || vi.FOV > 170.f) vi.FOV = 90.f;

            // Prefer the real POV, resolved by PCOwner during the startup scan.
            uintptr_t camMgr = g_cameraMgr;
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
        int localteam = -1;
        if (ackpawn) {
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

        EntityData tempEnts[kMaxEntities] = {};
        int count = 0;
        // Why does a roster of 12 become one box? Six `continue`s can drop a
        // player and none of them said so. Count each: the answer is which
        // number is 11, not a guess about which offset moved.
        int dropNoPs = 0, dropSpec = 0, dropNoPawn = 0, dropNoRoot = 0,
            dropZeroPos = 0, dropDist = 0, dropDead = 0;

        for (int i = 0; i < playerCount && count < kMaxEntities; i++) {
            uintptr_t ps = psArr[i];
            if (!ps) { dropNoPs++; continue; }

            if (g_off.APlayerState_Spectator) {
                uint8_t flags = mem.read<uint8_t>(ps + g_off.APlayerState_Spectator);
                if (flags & (1 << 1)) { dropSpec++; continue; }
            }

            uintptr_t pawn = mem.readPtr(ps + g_off.APlayerState_PawnPrivate);
            if (!pawn) { dropNoPawn++; continue; }

            EntityData& ent = tempEnts[count];
            ent.isSelf = (pawn == ackpawn);

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

            // Health - CurrentHealth and HealthMax are doubles in this SDK
            uintptr_t hc = mem.readPtr(pawn + g_off.ADiscoveryCharacter_Health);
            if (hc) {
                float a = mem.read<float>(hc + g_off.Health_A);
                float b = mem.read<float>(hc + g_off.Health_B);
                if (std::isfinite(a) && std::isfinite(b) &&
                    a >= 0.f && b >= 0.f && a <= 1000.f && b <= 1000.f) {
                    ent.health    = (a < b) ? a : b;     // current is the smaller
                    ent.maxHealth = (a < b) ? b : a;
                    if (ent.maxHealth <= 0.0) ent.maxHealth = 100.0;
                    if (ent.health <= 0.0) { dropDead++; continue; }
                }
            }

            // Squad
            uintptr_t sc = mem.readPtr(pawn + g_off.ADiscoveryCharacter_Squad);
            if (sc) ent.squadIdx = mem.read<int32_t>(sc + g_off.Squad_Index);
            // localteam is read from the local pawn each tick above. Squad index
            // is confirmed correct in every mode, including squads larger than 3.
            ent.isTeammate = (localteam >= 0 && ent.squadIdx == localteam && !ent.isSelf);

            // Name. PlayerNamePrivate is marked "GARBAGE on this game, unused"
            // in offsets.hpp and was still being read here; the diagnostic dump
            // uses DisplayName + Discriminator, which is what actually resolves.
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

        {
            static const bool eDbg = getenv("ESP_DEBUG") != nullptr;
            static int dbgN = 0;
            if (eDbg && (dbgN++ % 120) == 0)
                printf("[ents] roster %d -> kept %d   dropped: noPS %d  spectator %d  "
                       "noPawn %d  noRoot %d  zeroPos %d  tooFar %d  dead %d\n",
                       playerCount, count, dropNoPs, dropSpec, dropNoPawn,
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
        }

        // ~60 Hz
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto sleep   = std::chrono::milliseconds(16) - elapsed;
        if (sleep.count() > 0)
            std::this_thread::sleep_for(sleep);
    }

    printf("[reader] thread done\n");
}
