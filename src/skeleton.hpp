#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Bone hierarchy. Composes the mesh's parent-relative bone array
// into world space so the skeleton can be drawn and aimed at.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>
#include "mem.hpp"
#include "structs.hpp"
#include "runtime_offsets.hpp"

// THE CHAIN. Three of four hops are published by reflection:

namespace skel {
constexpr size_t kTransform = 96;   // Rotation +0x00, Translation +0x20, Scale +0x40
constexpr size_t kRotOff    = 0x00;
constexpr size_t kPosOff    = 0x20;

struct Rig {
    bool      ok      = false;
    uintptr_t skeleton = 0;
    int       count   = 0;
    std::vector<int32_t> parents;
    std::vector<FVector> bind;      // component-space bind positions
    // named from the bind pose, never hardcoded
    int head = -1, neck = -1, chest = -1, pelvis = -1, lFoot = -1, rFoot = -1;
    float bindHeight = 0.f;

    int headIdx = -1;
    std::map<int, int> headVotes;
    int headSamples = 0;
};

inline FVector qrot(const FQuat& q, const FVector& v) {
    const double ux = q.Y*v.Z - q.Z*v.Y;
    const double uy = q.Z*v.X - q.X*v.Z;
    const double uz = q.X*v.Y - q.Y*v.X;
    const double wx = q.Y*uz - q.Z*uy;
    const double wy = q.Z*ux - q.X*uz;
    const double wz = q.X*uy - q.Y*ux;
    return FVector{ v.X + 2.0*(q.W*ux + wx),
                    v.Y + 2.0*(q.W*uy + wy),
                    v.Z + 2.0*(q.W*uz + wz) };
}

// UE order: C = Parent * Local applies Local first, then Parent.
inline FQuat qmul(const FQuat& a, const FQuat& b) {
    FQuat r;
    r.X = a.W*b.X + a.X*b.W + a.Y*b.Z - a.Z*b.Y;
    r.Y = a.W*b.Y - a.X*b.Z + a.Y*b.W + a.Z*b.X;
    r.Z = a.W*b.Z + a.X*b.Y - a.Y*b.X + a.Z*b.W;
    r.W = a.W*b.W - a.X*b.X - a.Y*b.Y - a.Z*b.Z;
    return r;
}

// Compose one bone into COMPONENT space by walking up to the root.
inline bool composeOne(const std::vector<uint8_t>& raw, int count,
                       const std::vector<int32_t>& par, int idx, FVector& out) {
    int chain[128];
    int n = 0;
    for (int i = idx; i >= 0 && n < 128; i = par[i]) chain[n++] = i;
    FVector pos{0, 0, 0};
    FQuat   rot{0, 0, 0, 1};
    for (int k = n - 1; k >= 0; --k) {
        const int b = chain[k];
        if (b >= count) return false;
        FQuat   q;  std::memcpy(&q, raw.data() + size_t(b)*kTransform + kRotOff, sizeof(FQuat));
        FVector t;  std::memcpy(&t, raw.data() + size_t(b)*kTransform + kPosOff, sizeof(FVector));
        const FVector r = qrot(rot, t);
        pos.X += r.X; pos.Y += r.Y; pos.Z += r.Z;
        rot = qmul(rot, q);
    }
    out = pos;
    return true;
}

inline bool readArray(const Mem& mem, uintptr_t addr, uintptr_t& data, int32_t& num) {
    data = mem.readPtr(addr);
    num  = mem.read<int32_t>(addr + 8);
    const int32_t cap = mem.read<int32_t>(addr + 12);
    return data && num > 0 && num <= 4096 && cap >= num;
}

// The parent table: [0] == -1 and [i] < i, all the way down.
inline bool findParents(const Mem& mem, uintptr_t skeleton,
                        std::vector<int32_t>& out, uintptr_t& foundAt) {
    for (uintptr_t o = 0; o < 0x800; o += 8) {
        uintptr_t data; int32_t num;
        if (!readArray(mem, skeleton + o, data, num)) continue;
        if (num < 8 || num > 600) continue;
        std::vector<uint8_t> blk(size_t(num) * 12);
        if (!mem.read_raw(data, blk.data(), blk.size())) continue;
        std::vector<int32_t> p(num);
        bool good = true;
        for (int i = 0; i < num && good; ++i) {
            std::memcpy(&p[i], blk.data() + size_t(i)*12 + 8, 4);
            if (i == 0) good = (p[0] == -1);
            else        good = (p[i] >= 0 && p[i] < i);
        }
        if (!good) continue;
        int branches = 0;
        std::map<int,int> kids;
        for (int i = 1; i < num; ++i) if (++kids[p[i]] == 2) ++branches;
        if (branches < 3) continue;      // a rig has shoulders and hips
        out = std::move(p);
        foundAt = o;
        return true;
    }
    return false;
}

// The reference pose: the TArray<FTransform> of the same length, unit quaternions.
inline bool findBindPose(const Mem& mem, uintptr_t skeleton, int32_t want,
                         std::vector<uint8_t>& out) {
    for (uintptr_t o = 0; o < 0x800; o += 8) {
        uintptr_t data; int32_t num;
        if (!readArray(mem, skeleton + o, data, num)) continue;
        if (num != want || (data & 0xF)) continue;
        std::vector<uint8_t> raw(size_t(num) * kTransform);
        if (!mem.read_raw(data, raw.data(), raw.size())) continue;
        int good = 0, tested = std::min(num, 32);
        for (int i = 0; i < tested; ++i) {
            FQuat q; std::memcpy(&q, raw.data() + size_t(i)*kTransform + kRotOff, sizeof(FQuat));
            const double s = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
            if (s > 0.98 && s < 1.02) ++good;
        }
        if (good >= tested * 9 / 10) { out = std::move(raw); return true; }
    }
    return false;
}

// Name the bones FROM THE BIND POSE. Nothing here is an index typed in by hand:
// the spine is "from the root, always the child that gains the most height", the
// head is the last bone on it that still has children of its own (crown, hair),
// and the feet are the lowest bones under the hips.
inline void nameBones(Rig& r) {
    std::map<int, std::vector<int>> kids;
    for (int i = 1; i < r.count; ++i) kids[r.parents[i]].push_back(i);

    std::vector<int>    sub(r.count, 1);
    std::vector<double> peak(r.count);
    for (int i = 0; i < r.count; ++i) peak[i] = r.bind[i].Z;
    for (int i = r.count - 1; i > 0; --i) {         // children precede parents
        const int p = r.parents[i];
        sub[p] += sub[i];
        if (peak[i] > peak[p]) peak[p] = peak[i];
    }

    // the pelvis: the child of the root carrying the most of the body
    int pelvis = -1;
    for (int c : kids[0]) if (pelvis < 0 || sub[c] > sub[pelvis]) pelvis = c;
    if (pelvis < 0) return;
    r.pelvis = pelvis;

    // the spine: from the pelvis, always into the branch whose subtree REACHES
    // highest -- the arm branches top out at the shoulders, the head branch does
    // not. Never into a branch that cannot climb.
    std::vector<int> chain;
    int cur = pelvis;
    for (int step = 0; step < 24; ++step) {
        chain.push_back(cur);
        int best = -1;
        for (int c : kids[cur]) if (best < 0 || peak[c] > peak[best]) best = c;
        if (best < 0 || peak[best] <= r.bind[cur].Z + 0.5) break;
        cur = best;
    }

    // the head: the highest bone on that chain that still has children of its
    // own (the crown and the hair hang off it); the leaf above it is not a joint
    for (int i = int(chain.size()) - 1; i >= 0; --i) {
        const int b = chain[i];
        if (!kids[b].empty() && r.bind[b].Z > r.bind[pelvis].Z) {
            r.head = b;
            r.neck = r.parents[b];
            break;
        }
    }
    // the chest: the fork on the spine carrying the most branches (neck and both
    // clavicles), which is what makes it the chest rather than a spine segment
    size_t bestKids = 0;
    for (int b : chain)
        if (b != r.head && kids[b].size() > bestKids) { bestKids = kids[b].size(); r.chest = b; }

    // the feet: the two lowest bones that are NOT in the same leg
    int f1 = -1, f2 = -1;
    for (int i = 1; i < r.count; ++i) {
        if (f1 < 0 || r.bind[i].Z < r.bind[f1].Z) f1 = i;
    }
    if (f1 > 0) {
        // walk up from f1 to the child of the pelvis that owns it, then take the
        // lowest bone in a DIFFERENT pelvis branch
        int legA = f1;
        while (legA >= 0 && r.parents[legA] != pelvis) legA = r.parents[legA];
        for (int i = 1; i < r.count; ++i) {
            int up = i;
            while (up >= 0 && r.parents[up] != pelvis) up = r.parents[up];
            if (up == legA || up < 0) continue;
            if (f2 < 0 || r.bind[i].Z < r.bind[f2].Z) f2 = i;
        }
    }
    r.lFoot = f1; r.rFoot = (f2 >= 0 ? f2 : f1);
}

// Build (and cache) the rig behind a mesh component.
inline Rig* rigFor(const Mem& mem, uintptr_t mesh) {
    static std::map<uintptr_t, Rig> cache;
    if (!mesh) return nullptr;

    if (g_off.Mesh_BoneTree && g_off.BoneTree_Parents) {
        const uintptr_t owner = mem.readPtr(mesh + g_off.Mesh_BoneTree);
        if (owner && !(owner & 7)) {
            auto ito = cache.find(owner);
            if (ito != cache.end())
                return ito->second.ok ? &ito->second : nullptr;
            Rig m;
            uintptr_t data; int32_t num;
            if (readArray(mem, owner + g_off.BoneTree_Parents, data, num)
                && num >= 8 && num <= 600) {
                // NOT `std::vector<int32_t> p(size_t(num));` -- that is a
                // most-vexing-parse and compiles as a function declaration.
                std::vector<int32_t> p;
                p.resize(static_cast<size_t>(num));
                if (mem.read_raw(data, p.data(), size_t(num) * 4)) {
                    bool ok = (p[0] == -1);
                    for (int i = 1; i < num && ok; ++i)
                        ok = (p[i] >= 0 && p[i] < i);
                    if (ok) {
                        m.skeleton = owner;
                        m.parents  = std::move(p);
                        m.count    = num;
                        m.head   = (g_off.Bone_Head &&
                                    int(g_off.Bone_Head) < num)
                                 ? int(g_off.Bone_Head) : -1;
                        m.pelvis = (g_off.Bone_Pelvis &&
                                    int(g_off.Bone_Pelvis) < num)
                                 ? int(g_off.Bone_Pelvis) : -1;
                        m.ok = true;
                        printf("[skel] mesh hierarchy: %d bones at "
                               "mesh+0x%lX -> +0x%lX (head %d, pelvis %d)\n",
                               num, (unsigned long)g_off.Mesh_BoneTree,
                               (unsigned long)g_off.BoneTree_Parents,
                               m.head, m.pelvis);
                        cache[owner] = std::move(m);
                        return &cache[owner];
                    }
                }
            }
            printf("[skel] Mesh_BoneTree 0x%lX -> +0x%lX did not validate -- "
                   "falling back to the skeleton table\n",
                   (unsigned long)g_off.Mesh_BoneTree,
                   (unsigned long)g_off.BoneTree_Parents);
        }
    }

    if (!g_off.Mesh_SkeletalMeshAsset || !g_off.SkeletalMesh_Skeleton)
        return nullptr;
    const uintptr_t asset = mem.readPtr(mesh + g_off.Mesh_SkeletalMeshAsset);
    if (!asset || (asset & 7)) return nullptr;
    const uintptr_t skeleton = mem.readPtr(asset + g_off.SkeletalMesh_Skeleton);
    if (!skeleton || (skeleton & 7)) return nullptr;

    auto it = cache.find(skeleton);
    if (it != cache.end()) return it->second.ok ? &it->second : nullptr;

    Rig r;
    r.skeleton = skeleton;
    uintptr_t at = 0;

    bool fromConfig = false;
    if (g_off.Skeleton_ParentTable) {
        uintptr_t data; int32_t num;
        if (readArray(mem, skeleton + g_off.Skeleton_ParentTable, data, num)
            && num >= 8 && num <= 600) {
            std::vector<uint8_t> blk(size_t(num) * 12);
            if (mem.read_raw(data, blk.data(), blk.size())) {
                r.parents.resize(num);
                bool ok = true;
                for (int i = 0; i < num && ok; ++i) {
                    std::memcpy(&r.parents[i], blk.data() + size_t(i)*12 + 8, 4);
                    ok = (i == 0) ? (r.parents[0] == -1)
                                  : (r.parents[i] >= 0 && r.parents[i] < i);
                }
                if (ok) { at = g_off.Skeleton_ParentTable; fromConfig = true; }
                else    { r.parents.clear(); }
            }
        }
        if (!fromConfig)
            printf("[skel] Skeleton_ParentTable 0x%lX did not validate -- scanning\n",
                   (unsigned long)g_off.Skeleton_ParentTable);
    }
    if (!fromConfig && !findParents(mem, skeleton, r.parents, at)) {
        cache[skeleton] = r; return nullptr;
    }
    r.count = int(r.parents.size());

    std::vector<uint8_t> bindRaw;
    if (!findBindPose(mem, skeleton, r.count, bindRaw)) { cache[skeleton] = r; return nullptr; }

    r.bind.resize(r.count);
    for (int i = 0; i < r.count; ++i)
        if (!composeOne(bindRaw, r.count, r.parents, i, r.bind[i])) {
            cache[skeleton] = r; return nullptr;
        }

    // THE CONTROL: the reference pose must be a person before anything is named.
    double lo = 1e9, hi = -1e9, xlo = 1e9, xhi = -1e9;
    for (int i = 0; i < r.count; ++i) {
        lo = std::min(lo, r.bind[i].Z); hi = std::max(hi, r.bind[i].Z);
        xlo = std::min(xlo, r.bind[i].X); xhi = std::max(xhi, r.bind[i].X);
    }
    r.bindHeight = float(hi - lo);
    if (r.bindHeight < 120.f || r.bindHeight > 260.f || (xhi - xlo) < 40.0) {
        printf("[skel] reference pose is not a person (%.0f tall, %.0f wide) -- "
               "rig rejected\n", r.bindHeight, xhi - xlo);
        cache[skeleton] = r;
        return nullptr;
    }
    nameBones(r);
    if (g_off.Bone_Head && int(g_off.Bone_Head) < r.count) {
        r.headIdx = int(g_off.Bone_Head);      // derived offline, no voting needed
        printf("[skel] head joint = bone %d (from offsets.cfg)\n", r.headIdx);
    }
    if (r.head < 0 || r.chest < 0 || r.pelvis < 0) { cache[skeleton] = r; return nullptr; }
    r.ok = true;
    printf("[skel] skeleton 0x%lx: %d bones, table at +0x%lX%s, bind pose %.0f tall\n",
           (unsigned long)skeleton, r.count, (unsigned long)at,
           fromConfig ? " (from offsets.cfg)" : " (scanned)", r.bindHeight);
    printf("[skel]   head %d  neck %d  chest %d  pelvis %d  feet %d/%d "
           "(named from the bind pose)\n", r.head, r.neck, r.chest, r.pelvis,
           r.lFoot, r.rFoot);
    cache[skeleton] = r;
    return &cache[skeleton];
}

// The head JOINT, found geometrically -- no bone index, no naming.
inline bool headLateral(const Rig& rig, const std::vector<uint8_t>& raw,
                        int liveCount, double targetZ, double lateralMax,
                        FVector& out) {
    if (liveCount < 8) return false;
    bool found = false;
    double bestDz = 1e18;
    FVector best{};
    for (int i = 0; i < liveCount && i < rig.count; ++i) {
        FVector p;
        if (!composeOne(raw, liveCount, rig.parents, i, p)) continue;
        if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z))
            continue;
        const double lat = std::sqrt(p.X*p.X + p.Y*p.Y);
        if (lat < 0.001) continue;          // the rigid marker: it cannot track
        if (lat > lateralMax) continue;     // an arm, or a weapon
        const double dz = std::fabs(p.Z - targetZ);
        if (dz > lateralMax) continue;      // nowhere near the head
        if (!found || dz < bestDz) { bestDz = dz; best = p; found = true; }
    }
    if (found) out = best;
    return found;
}

inline bool composeOneOriented(const std::vector<uint8_t>& raw, int count,
                               const std::vector<int32_t>& par, int idx,
                               FVector& outPos, FQuat& outRot) {
    int chain[128];
    int n = 0;
    for (int i = idx; i >= 0 && n < 128; i = par[i]) chain[n++] = i;
    FVector pos{0, 0, 0};
    FQuat   rot{0, 0, 0, 1};
    for (int k = n - 1; k >= 0; --k) {
        const int b = chain[k];
        if (b >= count) return false;
        FQuat   q;  std::memcpy(&q, raw.data() + size_t(b)*kTransform + kRotOff, sizeof(FQuat));
        FVector t;  std::memcpy(&t, raw.data() + size_t(b)*kTransform + kPosOff, sizeof(FVector));
        const FVector r = qrot(rot, t);
        pos.X += r.X; pos.Y += r.Y; pos.Z += r.Z;
        rot = qmul(rot, q);
    }
    outPos = pos; outRot = rot;
    return true;
}

inline bool boneWorld(const Mem& mem, uintptr_t mesh, const Rig& rig, int idx,
                      const std::vector<uint8_t>& liveRaw, int liveCount,
                      const FQuat& ctwRot, const FVector& ctwPos,
                      const FVector& ctwScale, FVector& out) {
    if (idx < 0 || idx >= liveCount) return false;
    FVector local;
    if (!composeOne(liveRaw, liveCount, rig.parents, idx, local)) return false;
    const FVector scaled{ local.X * ctwScale.X, local.Y * ctwScale.Y,
                          local.Z * ctwScale.Z };
    const FVector r = qrot(ctwRot, scaled);
    out = FVector{ r.X + ctwPos.X, r.Y + ctwPos.Y, r.Z + ctwPos.Z };
    return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}

}  // namespace skel
