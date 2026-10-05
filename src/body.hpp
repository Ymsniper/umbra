#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// A player's own body: the mesh the game draws, posed the way it is drawn.
//
// The tubes in hitbox.hpp are a person's proportions laid over the bones. This
// is the person: the vertices of the mesh the game renders, read once per asset
// and moved every frame with the bones the skeleton already reads. The outline
// is drawn from it, and the trigger tests the shot's line against it.
//
// WHERE IT COMES FROM. The asset's render data holds its levels of detail, each
// with a buffer of vertex positions in the pose the mesh was modelled in, and
// the asset keeps, for every bone, the inverse of where that bone stood in that
// pose. Together they say where every vertex sits relative to every bone.
//
// HOW IT MOVES. The game moves a vertex by a blend of a few bones, with weights
// it stores per vertex. Those weights are not read here. Instead a vertex
// follows the bone it lies on in the modelled pose, the way skin follows the
// bone under it, and near a joint it is blended between the two bones it lies
// between, which is what the game's own weights do there as well. Limbs, torso
// and head come out where the game draws them; what differs is how the skin on
// a bent joint creases, which is a centimetre or two at an elbow.
//
// WHICH BONES. Only bones that hang under the hips can carry a body. The root,
// and whatever hangs straight off it, are animation helpers, and some of those
// never move with the body at all. A level made of a single section also names
// the bones that actually carry it, and then only those are used.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "mem.hpp"
#include "structs.hpp"
#include "skeleton.hpp"
#include "runtime_offsets.hpp"
#include "hitbox.hpp"

namespace body {

// What part of a person a vertex is, for shooting at one part and not another.
enum Part : uint8_t { kNone = 0, kHead, kNeck, kChest, kBelly, kArm, kLeg };

// The level of detail closest to this many vertices without going over is the
// one used. A silhouette is made of a few thousand points as surely as of fifty
// thousand, and every vertex is moved every frame.
inline constexpr int kVertexBudget = 16000;

struct Model {
    uintptr_t asset = 0;
    uintptr_t renderData = 0;      // what the asset pointed at when it was read
    int  lod = 0, lods = 0;
    int  bones = 0;
    int  verts = 0;
    bool classified = false;       // every vertex has a part
    std::vector<float>    bind;    // 3 per vertex, the modelled pose
    std::vector<uint16_t> b0, b1;  // the bone a vertex follows, and its neighbour
    std::vector<uint8_t>  w1;      // the neighbour's share, out of 255
    std::vector<uint8_t>  part;    // Part of each vertex
    std::vector<float>    inv;     // 12 per bone: the 3x3 then the translation
    std::vector<uint8_t>  used;    // 1 for a bone some vertex follows
    std::vector<float>    rad;     // how far each vertex's surface reaches, cm
    float radMax  = 0.f;
    float spacing = 2.f;           // the usual gap to the nearest vertex, cm
    float height  = 0.f;           // of the modelled pose, cm
    bool  armsDown = false;        // modelled with its arms against its body
};

// One player's body this frame, in world space.
struct Body {
    uintptr_t id = 0;              // EntityData::id
    std::shared_ptr<const Model> model;
    std::vector<float> xyz;        // 3 per vertex
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
};

struct Frame {
    std::vector<Body> bodies;
    int n = 0;                     // bodies[0..n) are this frame's
};

// ── the arithmetic, kept apart from memory so it can be checked offline ───────

// A row-major 4x4 as the engine stores FMatrix44f: a point is a row vector on
// the left, so the translation is the last row.
inline bool affine(const float* m) {
    for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
    return std::fabs(m[3]) < 1e-3f && std::fabs(m[7]) < 1e-3f
        && std::fabs(m[11]) < 1e-3f && std::fabs(m[15] - 1.f) < 1e-3f;
}

// Where a bone stood in the modelled pose, from the inverse of that pose: the
// point the inverse sends to the origin.
inline bool boneFromInverse(const float* m, double out[3]) {
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-9) return false;
    const double inv[9] = {
        (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
        (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
        (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det };
    const double t[3] = { m[12], m[13], m[14] };
    for (int j = 0; j < 3; j++)
        out[j] = -(t[0] * inv[0 * 3 + j] + t[1] * inv[1 * 3 + j] + t[2] * inv[2 * 3 + j]);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

inline double segDist(const double p[3], const double a[3], const double b[3],
                      double& t) {
    const double v[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const double w[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const double vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    t = vv > 1e-12 ? (w[0] * v[0] + w[1] * v[1] + w[2] * v[2]) / vv : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    const double d[3] = { w[0] - v[0] * t, w[1] - v[1] * t, w[2] - v[2] * t };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// The part each bone carries, read off the same classification the tubes use.
// A bone is what its nearest classified ancestor is, and anything under the
// head is head: the jaw and the eyes are as much a headshot as the skull.
inline std::vector<uint8_t> boneParts(const skel::Rig& rig, bool& ok) {
    std::vector<uint8_t> out(rig.count, kNone);
    const hit::Skin* sk = hit::skinFor(&rig);
    ok = sk != nullptr;
    if (!sk) return out;
    for (int b = 0; b < rig.count; b++) {
        int x = b;
        for (int guard = 0; x > 0 && guard < 64; guard++) {
            if (x == sk->head || sk->cls[x] == hit::cHead) { out[b] = kHead; break; }
            const uint8_t c = sk->cls[x];
            if (c == hit::cNeck)  { out[b] = kNeck; break; }
            // A spine bone carries the stretch of torso above it, so the one
            // just under the collarbones' fork already holds the chest.
            if (c == hit::cTorso) {
                out[b] = sk->spineAt[x] >= sk->chestAt - 1 ? kChest : kBelly;
                break;
            }
            if (c == hit::cArm) { out[b] = kArm; break; }
            if (c == hit::cLeg) { out[b] = kLeg; break; }
            x = rig.parents[x];
        }
    }
    return out;
}

// Ties every vertex to the bones it follows. `pos` is the modelled pose, three
// floats a vertex; `mats` is one inverse pose matrix per bone; `allow`, when
// not empty, is the set of bones the mesh says carry it.
inline bool assemble(Model& m, const std::vector<float>& pos,
                     const std::vector<float>& mats, const skel::Rig& rig,
                     const std::vector<uint8_t>& allow, std::string& why) {
    const int n = rig.count;
    const int V = (int)(pos.size() / 3);
    if (V < 300) { why = "too few vertices to be a body"; return false; }
    if ((int)(mats.size() / 16) != n) { why = "one pose matrix per bone is needed"; return false; }

    // THE CONTROL: the modelled pose has to be a person before it is posed.
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int v = 0; v < V; v++)
        for (int k = 0; k < 3; k++) {
            lo[k] = std::min(lo[k], pos[v * 3 + k]);
            hi[k] = std::max(hi[k], pos[v * 3 + k]);
        }
    m.height = hi[2] - lo[2];
    const float wide = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (m.height < 100.f || m.height > 260.f || wide < 30.f || wide > 300.f) {
        char b[128];
        snprintf(b, sizeof b, "the modelled pose is not a person (%.0f tall, %.0f wide)",
                 m.height, wide);
        why = b; return false;
    }

    std::vector<std::array<double, 3>> P(n);
    m.inv.assign((size_t)n * 12, 0.f);
    for (int b = 0; b < n; b++) {
        const float* M = &mats[(size_t)b * 16];
        if (!affine(M)) { why = "a bone's pose matrix is not a transform"; return false; }
        double p[3];
        if (!boneFromInverse(M, p)) { why = "a bone's pose matrix cannot be inverted"; return false; }
        P[b] = { p[0], p[1], p[2] };
        float* o = &m.inv[(size_t)b * 12];
        o[0] = M[0]; o[1] = M[1]; o[2]  = M[2];
        o[3] = M[4]; o[4] = M[5]; o[5]  = M[6];
        o[6] = M[8]; o[7] = M[9]; o[8]  = M[10];
        o[9] = M[12]; o[10] = M[13]; o[11] = M[14];
    }

    // The skeleton and the mesh have to be the same body: the hips and the head
    // where a person keeps them, and both inside what the vertices span.
    const int hip = rig.pelvis, head = rig.head;
    if (hip < 0 || hip >= n || head < 0 || head >= n) {
        why = "the skeleton has no hips and head to measure by"; return false;
    }
    const double S = std::sqrt(std::pow(P[head][0] - P[hip][0], 2) +
                               std::pow(P[head][1] - P[hip][1], 2) +
                               std::pow(P[head][2] - P[hip][2], 2));
    if (S < 0.2 * m.height || S > 0.6 * m.height || P[head][2] <= P[hip][2]) {
        char b[160];
        snprintf(b, sizeof b, "the skeleton's hips and head are %.0f cm apart on a "
                 "%.0f cm mesh, which is not the same body", S, m.height);
        why = b; return false;
    }

    // Which bones may carry a vertex: under the hips, inside the mesh, and named
    // by the mesh when it names any.
    std::vector<uint8_t> cand(n, 0);
    for (int b = 0; b < n; b++) {
        int x = b;
        for (int guard = 0; x >= 0 && guard < 128; guard++) {
            if (x == hip) { cand[b] = 1; break; }
            x = rig.parents[x];
        }
        if (!cand[b]) continue;
        if (!allow.empty() && !allow[b]) { cand[b] = 0; continue; }
        // A bone standing outside the body it is meant to carry is a socket for
        // something else, wherever it hangs.
        for (int k = 0; k < 3; k++)
            if (P[b][k] < lo[k] - 15.0 || P[b][k] > hi[k] + 15.0) cand[b] = 0;
    }
    if (!cand[hip] || !cand[head]) { why = "the hips or the head cannot carry the mesh"; return false; }

    struct Seg { int bone; double a[3], b[3]; bool leaf; };
    std::vector<Seg> segs;
    for (int b = 0; b < n; b++) {
        if (!cand[b]) continue;
        bool any = false;
        for (int c = b + 1; c < n; c++) {
            if (rig.parents[c] != b || !cand[c]) continue;
            const double len = std::sqrt(std::pow(P[c][0] - P[b][0], 2) +
                                         std::pow(P[c][1] - P[b][1], 2) +
                                         std::pow(P[c][2] - P[b][2], 2));
            if (len < 0.5) continue;
            segs.push_back({ b, { P[b][0], P[b][1], P[b][2] },
                                { P[c][0], P[c][1], P[c][2] }, false });
            any = true;
        }
        if (any) continue;
        // A bone with nothing after it still carries what carries on past it:
        // a hand past the wrist, a skull past the top of the neck. So it reaches
        // on the way the limb was going, half as far again as the bone before
        // it. As a bare point it would lose its own fingertips to whatever
        // limb happens to hang beside them.
        Seg s{ b, { P[b][0], P[b][1], P[b][2] }, { P[b][0], P[b][1], P[b][2] }, true };
        const int p = rig.parents[b];
        if (p >= 0) {
            const double d[3] = { P[b][0] - P[p][0], P[b][1] - P[p][1], P[b][2] - P[p][2] };
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0.5) {
                const double reach = std::clamp(0.5 * len, 3.0, 20.0) / len;
                for (int k = 0; k < 3; k++) s.b[k] = P[b][k] + d[k] * reach;
            }
        }
        segs.push_back(s);
    }

    // Past the end of a bone is the next bone's, so the parent is made to lose
    // the tie it would otherwise share with the child at their joint.
    const double pastEnd = 1.0;
    // How far either side of the line between two bones the skin is shared.
    const double W = std::clamp(0.05 * S, 1.5, 6.0);

    bool classOk = false;
    const std::vector<uint8_t> parts = boneParts(rig, classOk);
    m.classified = classOk;
    m.bind = pos;
    m.verts = V;
    m.bones = n;
    m.b0.assign(V, 0); m.b1.assign(V, 0); m.w1.assign(V, 0); m.part.assign(V, kNone);
    m.used.assign(n, 0);
    int near = 0;
    for (int v = 0; v < V; v++) {
        const double p[3] = { pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2] };
        double d0 = 1e18, d1 = 1e18;
        int bb0 = -1, bb1 = -1;
        for (const Seg& s : segs) {
            double t;
            const double d = segDist(p, s.a, s.b, t) + ((!s.leaf && t > 0.999) ? pastEnd : 0.0);
            if (s.bone == bb0) { if (d < d0) d0 = d; continue; }
            if (s.bone == bb1) {
                if (d < d1) d1 = d;
                if (d1 < d0) { std::swap(d0, d1); std::swap(bb0, bb1); }
                continue;
            }
            if (d < d0) { d1 = d0; bb1 = bb0; d0 = d; bb0 = s.bone; }
            else if (d < d1) { d1 = d; bb1 = s.bone; }
        }
        if (bb0 < 0) { why = "a vertex has no bone to follow"; return false; }
        if (d0 < 0.45 * S) near++;
        double share = 0.0;
        if (bb1 >= 0 && d1 - d0 < W) share = 0.5 * (1.0 - (d1 - d0) / W);
        m.b0[v] = (uint16_t)bb0;
        m.b1[v] = (uint16_t)(bb1 >= 0 ? bb1 : bb0);
        m.w1[v] = (uint8_t)std::lround(share * 255.0);
        m.part[v] = parts[bb0];
        m.used[bb0] = 1;
        if (bb1 >= 0) m.used[bb1] = 1;
    }
    // A body's skin lies close to its bones. If a large share of it does not,
    // the mesh and the skeleton are not describing the same thing.
    if (near < V * 9 / 10) {
        char b[160];
        snprintf(b, sizeof b, "only %d of %d vertices lie near a bone, so the mesh "
                 "does not sit on this skeleton", near, V);
        why = b; return false;
    }

    // Arms modelled hanging against the body share skin with it wherever they
    // touch, and without the game's own weights nothing here can tell which of
    // the two a touching vertex belongs to. Characters are modelled with their
    // arms out for exactly that reason, so this only says so when one is not.
    m.armsDown = false;
    if (const hit::Skin* sk = hit::skinFor(&rig)) {
        for (int b = 0; b < n && !m.armsDown; b++) {
            if (!cand[b] || sk->cls[b] != hit::cArm || sk->step[b] < 3) continue;
            const double p[3] = { P[b][0], P[b][1], P[b][2] };
            for (const Seg& s : segs) {
                const uint8_t pc = parts[s.bone];
                if (pc != kLeg && pc != kBelly && pc != kChest) continue;
                double t;
                if (segDist(p, s.a, s.b, t) < 0.3 * S) { m.armsDown = true; break; }
            }
        }
    }

    // How much surface each vertex stands for. A mesh is dense where the detail
    // is, a face or a hand, and sparse where it is not, a back or a thigh, and
    // it can be sparser one way than the other, so one figure for the whole
    // body leaves holes wherever it is sparse: holes the outline would trace
    // round from the inside and a shot would slip through. So each vertex is
    // measured on its own, to its sixth nearest neighbour, which is far enough
    // round it to reach across the widest gap beside it. Seams repeat a vertex
    // in the same place, so a neighbour that close is no neighbour.
    const double cell = 3.0;
    std::unordered_map<uint64_t, std::vector<int>> grid;
    grid.reserve((size_t)V);
    auto key = [&](int x, int y, int z) {
        return ((uint64_t)(uint32_t)(x + 100000) << 42) ^
               ((uint64_t)(uint32_t)(y + 100000) << 21) ^ (uint64_t)(uint32_t)(z + 100000);
    };
    for (int v = 0; v < V; v++)
        grid[key((int)std::floor(pos[v * 3] / cell), (int)std::floor(pos[v * 3 + 1] / cell),
                 (int)std::floor(pos[v * 3 + 2] / cell))].push_back(v);
    constexpr int K = 6;
    m.rad.assign(V, 0.f);
    std::vector<float> firsts;
    firsts.reserve((size_t)V);
    for (int v = 0; v < V; v++) {
        const int cx = (int)std::floor(pos[v * 3] / cell);
        const int cy = (int)std::floor(pos[v * 3 + 1] / cell);
        const int cz = (int)std::floor(pos[v * 3 + 2] / cell);
        double best[K];
        // Anything within `reach` cells of the vertex's own is certain to have
        // been seen, so a sixth neighbour further out than that is looked for
        // again over a wider block before it is believed.
        for (int reach = 1; reach <= 2; reach++) {
            for (double& b : best) b = 1e18;
            for (int dx = -reach; dx <= reach; dx++)
                for (int dy = -reach; dy <= reach; dy++)
                    for (int dz = -reach; dz <= reach; dz++) {
                        auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                        if (it == grid.end()) continue;
                        for (int u : it->second) {
                            if (u == v) continue;
                            const double ex = pos[u * 3] - pos[v * 3];
                            const double ey = pos[u * 3 + 1] - pos[v * 3 + 1];
                            const double ez = pos[u * 3 + 2] - pos[v * 3 + 2];
                            const double d = ex * ex + ey * ey + ez * ez;
                            if (d < 0.05 * 0.05 || d >= best[K - 1]) continue;
                            int at = K - 1;
                            while (at > 0 && best[at - 1] > d) { best[at] = best[at - 1]; at--; }
                            best[at] = d;
                        }
                    }
            if (best[K - 1] <= (reach * cell) * (reach * cell)) break;
        }
        const double dk = best[K - 1] < 1e17 ? std::sqrt(best[K - 1]) : 2.0 * cell;
        m.rad[v] = (float)std::clamp(0.65 * dk, 0.3, 6.0);
        if (best[0] < 1e17) firsts.push_back((float)std::sqrt(best[0]));
        m.radMax = std::max(m.radMax, m.rad[v]);
    }
    if (!firsts.empty()) {
        std::nth_element(firsts.begin(), firsts.begin() + firsts.size() / 2, firsts.end());
        m.spacing = std::clamp(firsts[firsts.size() / 2], 0.3f, 8.f);
    }
    return true;
}

// Every bone in component space, from the parent-relative transforms the game
// keeps, in the same composition the skeleton uses.
inline void compose(const std::vector<uint8_t>& raw, int num,
                    const std::vector<int32_t>& parents,
                    std::vector<FQuat>& R, std::vector<FVector>& P) {
    R.resize(num); P.resize(num);
    for (int i = 0; i < num; i++) {
        FQuat q; FVector t;
        std::memcpy(&q, raw.data() + (size_t)i * skel::kTransform + skel::kRotOff, sizeof q);
        std::memcpy(&t, raw.data() + (size_t)i * skel::kTransform + skel::kPosOff, sizeof t);
        const int p = (i < (int)parents.size()) ? parents[i] : -1;
        if (p < 0 || p >= i) { R[i] = q; P[i] = t; continue; }
        R[i] = skel::qmul(R[p], q);
        const FVector r = skel::qrot(R[p], t);
        P[i] = FVector(P[p].X + r.X, P[p].Y + r.Y, P[p].Z + r.Z);
    }
}

inline void quatMatrix(const FQuat& q, double m[9]) {
    const double x = q.X, y = q.Y, z = q.Z, w = q.W;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}

// The body in world space. A vertex goes into the space of the bone it
// follows by that bone's inverse modelled pose, comes back out by where the
// bone is now, and into the world by where the mesh is; the three are folded
// into one transform per bone before any vertex is touched. `R` and `P` are
// the bones in component space, as compose() leaves them.
inline bool pose(const Model& m, const std::vector<FQuat>& R,
                 const std::vector<FVector>& P, int num, const FQuat& cq,
                 const FVector& ct, const FVector& cs,
                 std::vector<float>& out, float lo[3], float hi[3]) {
    if (num != m.bones || (int)R.size() < num || (int)P.size() < num) return false;
    static thread_local std::vector<float> X;     // 12 per bone: 3x3 then offset
    X.assign((size_t)num * 12, 0.f);
    double Rc[9];
    quatMatrix(cq, Rc);
    const double sc[3] = { cs.X, cs.Y, cs.Z };
    double RcS[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) RcS[i * 3 + j] = Rc[i * 3 + j] * sc[j];
    for (int b = 0; b < num; b++) {
        if (!m.used[b]) continue;
        double Rb[9];
        quatMatrix(R[b], Rb);
        double M[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M[i * 3 + j] = RcS[i * 3 + 0] * Rb[0 * 3 + j] + RcS[i * 3 + 1] * Rb[1 * 3 + j]
                             + RcS[i * 3 + 2] * Rb[2 * 3 + j];
        const float* L = &m.inv[(size_t)b * 12];      // row-major, a point on the left
        float* o = &X[(size_t)b * 12];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                o[i * 3 + j] = (float)(M[i * 3 + 0] * L[j * 3 + 0] + M[i * 3 + 1] * L[j * 3 + 1]
                                     + M[i * 3 + 2] * L[j * 3 + 2]);
        const double T[3] = { L[9], L[10], L[11] };
        const double pb[3] = { P[b].X, P[b].Y, P[b].Z };
        for (int i = 0; i < 3; i++)
            o[9 + i] = (float)(M[i * 3 + 0] * T[0] + M[i * 3 + 1] * T[1] + M[i * 3 + 2] * T[2]
                             + RcS[i * 3 + 0] * pb[0] + RcS[i * 3 + 1] * pb[1]
                             + RcS[i * 3 + 2] * pb[2] + (i == 0 ? ct.X : i == 1 ? ct.Y : ct.Z));
    }
    out.resize((size_t)m.verts * 3);
    for (int k = 0; k < 3; k++) { lo[k] = 1e30f; hi[k] = -1e30f; }
    const float* B = m.bind.data();
    for (int v = 0; v < m.verts; v++) {
        const float x = B[v * 3], y = B[v * 3 + 1], z = B[v * 3 + 2];
        const float* a = &X[(size_t)m.b0[v] * 12];
        float px = a[0] * x + a[1] * y + a[2] * z + a[9];
        float py = a[3] * x + a[4] * y + a[5] * z + a[10];
        float pz = a[6] * x + a[7] * y + a[8] * z + a[11];
        if (m.w1[v]) {
            const float* c = &X[(size_t)m.b1[v] * 12];
            const float s = m.w1[v] * (1.f / 255.f);
            px += s * (c[0] * x + c[1] * y + c[2] * z + c[9]  - px);
            py += s * (c[3] * x + c[4] * y + c[5] * z + c[10] - py);
            pz += s * (c[6] * x + c[7] * y + c[8] * z + c[11] - pz);
        }
        out[v * 3] = px; out[v * 3 + 1] = py; out[v * 3 + 2] = pz;
        lo[0] = std::min(lo[0], px); hi[0] = std::max(hi[0], px);
        lo[1] = std::min(lo[1], py); hi[1] = std::max(hi[1], py);
        lo[2] = std::min(lo[2], pz); hi[2] = std::max(hi[2], pz);
    }
    return std::isfinite(lo[0]) && std::isfinite(hi[2]);
}

// The same, from the parent-relative transforms as the game keeps them.
inline bool pose(const Model& m, const std::vector<uint8_t>& raw, int num,
                 const std::vector<int32_t>& parents, const FQuat& cq,
                 const FVector& ct, const FVector& cs,
                 std::vector<float>& out, float lo[3], float hi[3]) {
    if (num != m.bones || (int)raw.size() < num * (int)skel::kTransform) return false;
    static thread_local std::vector<FQuat> R;
    static thread_local std::vector<FVector> P;
    compose(raw, num, parents, R, P);
    return pose(m, R, P, num, cq, ct, cs, out, lo, hi);
}

inline bool partWanted(int part, uint8_t p) {
    switch (part) {
        case hit::pHead:  return p == kHead;
        case hit::pChest: return p == kNeck || p == kChest;
        case hit::pBody:  return p == kNeck || p == kChest || p == kBelly;
        case hit::pLegs:  return p == kLeg;
        default:          return p != kNone;
    }
}

struct RayHit {
    bool   applicable = false;     // false: the mesh cannot answer for that part
    bool   hit = false;
    double range = 0.0;
    double gapPx = 0.0;
};

// Does the shot's line run through this body? The mesh is a surface sampled at
// its vertices, so the line has to pass within the reach of one of them, which
// is how much surface that vertex stands for; `padPx` widens that by pixels at
// its own range, as the forgiveness slider means. `lead` moves the body on by
// how far it has gone since the bones were read.
inline RayHit rayHit(const Body& b, const hit::Ray& ray, int part, double padPx,
                     const FVector& lead) {
    RayHit r;
    if (!ray.ok || !b.model) return r;
    const Model& m = *b.model;
    if (!m.classified && part != hit::pAll) return r;
    if ((int)m.rad.size() != m.verts) return r;
    r.applicable = true;
    const double ox = ray.o.X - lead.X, oy = ray.o.Y - lead.Y, oz = ray.o.Z - lead.Z;
    const double dx = ray.d.X, dy = ray.d.Y, dz = ray.d.Z;
    const double r0 = m.radMax;              // the most any vertex reaches

    // the whole body first, widened by the most any vertex can be
    {
        const double cxm = 0.5 * (b.lo[0] + b.hi[0]) - ox;
        const double cym = 0.5 * (b.lo[1] + b.hi[1]) - oy;
        const double czm = 0.5 * (b.lo[2] + b.hi[2]) - oz;
        const double reach = std::sqrt(cxm * cxm + cym * cym + czm * czm) + m.height;
        const double grow = r0 + padPx * reach * ray.pxAt1 + 1.0;
        double t0 = 0.0, t1 = 1e18;
        const double o3[3] = { ox, oy, oz }, d3[3] = { dx, dy, dz };
        for (int k = 0; k < 3; k++) {
            const double lo = b.lo[k] - grow, hi = b.hi[k] + grow;
            if (std::fabs(d3[k]) < 1e-12) {
                if (o3[k] < lo || o3[k] > hi) { r.gapPx = 1e9; return r; }
                continue;
            }
            double a = (lo - o3[k]) / d3[k], c = (hi - o3[k]) / d3[k];
            if (a > c) std::swap(a, c);
            t0 = std::max(t0, a); t1 = std::min(t1, c);
            if (t0 > t1) { r.gapPx = 1e9; return r; }
        }
    }

    double bestRange = 1e18, bestGap = 1e18;
    const float* X = b.xyz.data();
    for (int v = 0; v < m.verts; v++) {
        if (!partWanted(part, m.part[v]) && (m.classified || part != hit::pAll)) continue;
        const double wx = X[v * 3] - ox, wy = X[v * 3 + 1] - oy, wz = X[v * 3 + 2] - oz;
        const double t = wx * dx + wy * dy + wz * dz;
        if (t <= 1.0) continue;                          // at or behind the eye
        const double d2 = wx * wx + wy * wy + wz * wz - t * t;
        const double rr = m.rad[v] + (padPx > 0.0 ? padPx * t * ray.pxAt1 : 0.0);
        if (d2 <= rr * rr) {
            if (t < bestRange) bestRange = t;
        } else if (bestRange > 1e17) {
            const double gap = (std::sqrt(std::max(0.0, d2)) - rr) / (t * ray.pxAt1);
            if (gap < bestGap) bestGap = gap;
        }
    }
    if (bestRange < 1e17) { r.hit = true; r.range = bestRange; }
    else r.gapPx = bestGap < 1e17 ? bestGap : 1e9;
    return r;
}

inline const Body* find(const Frame& f, uintptr_t id) {
    for (int i = 0; i < f.n; i++) if (f.bodies[i].id == id) return &f.bodies[i];
    return nullptr;
}

// ── reading an asset, on a thread of its own ─────────────────────────────────

struct Builder {
    struct Job { const Mem* mem; uintptr_t asset; const skel::Rig* rig; };
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::thread worker;
    bool stop = false;
    std::map<uintptr_t, std::shared_ptr<const Model>> ready;
    std::map<uintptr_t, uintptr_t> failed;   // asset -> render data it failed with
    std::atomic<int> built{0}, refused{0};
};
inline Builder& builder() { static Builder b; return b; }

inline std::shared_ptr<const Model> read(const Mem& mem, uintptr_t asset,
                                         const skel::Rig& rig, std::string& why) {
    auto m = std::make_shared<Model>();
    m->asset = asset;
    const uintptr_t rd = mem.readPtr(asset + g_off.SkeletalMesh_RenderData);
    if (!rd || (rd & 7)) { why = "the asset has no render data"; return nullptr; }
    m->renderData = rd;
    const uintptr_t arr = mem.readPtr(rd + g_off.RenderData_LODRenderData);
    const int32_t lodNum = mem.read<int32_t>(rd + g_off.RenderData_LODRenderData + 8);
    if (!arr || lodNum < 1 || lodNum > 16) { why = "the render data lists no levels of detail"; return nullptr; }
    m->lods = lodNum;

    struct Lod { uintptr_t at = 0, data = 0; uint32_t stride = 0, verts = 0; };
    std::vector<Lod> L(lodNum);
    for (int i = 0; i < lodNum; i++) {
        L[i].at = mem.readPtr(arr + (uintptr_t)i * 8);
        if (!L[i].at || (L[i].at & 7)) continue;
        const uintptr_t pb = L[i].at + g_off.LOD_PositionBuffer;
        L[i].data   = mem.readPtr(pb + g_off.PosBuffer_Data);
        L[i].stride = mem.read<uint32_t>(pb + g_off.PosBuffer_Stride);
        L[i].verts  = mem.read<uint32_t>(pb + g_off.PosBuffer_NumVertices);
    }
    // A level whose vertices are not in memory has a buffer with nothing behind
    // it; the game streams the detailed ones in and out with distance.
    auto here = [](const Lod& l) {
        return l.data && !(l.data & 3) && l.verts >= 300 && l.verts <= 400000
            && l.stride >= 12 && l.stride <= 64;
    };
    // The most detailed level within the budget; failing that, the least
    // detailed one there is, thinned to the budget below.
    int pick = -1;
    for (int i = 0; i < lodNum && pick < 0; i++)
        if (here(L[i]) && (int)L[i].verts <= kVertexBudget) pick = i;
    if (pick < 0)
        for (int i = 0; i < lodNum; i++)
            if (here(L[i]) && (pick < 0 || L[i].verts < L[pick].verts)) pick = i;
    if (pick < 0) {
        // What each level held. A sensible stride and count with no data
        // behind them is the game having let go of its copy after upload;
        // nonsense in all three is the way to them reading wrong.
        std::string held;
        char b[112];
        for (int i = 0; i < lodNum && i < 6; i++) {
            snprintf(b, sizeof b, "%slevel %d at 0x%lx: data 0x%lx, stride %u, %u vertices",
                     i ? "; " : "", i, (unsigned long)L[i].at, (unsigned long)L[i].data,
                     L[i].stride, L[i].verts);
            held += b;
        }
        why = "no level of detail has its vertices in memory (" + held + ")";
        return nullptr;
    }
    m->lod = pick;
    const Lod& l = L[pick];

    std::vector<uint8_t> raw((size_t)l.verts * l.stride);
    if (!mem.read_raw(l.data, raw.data(), raw.size())) {
        why = "the vertex buffer could not be read"; return nullptr;
    }
    // Over the budget, every k-th vertex: the surface is still there, sampled
    // more coarsely, and the gap between samples is measured from what is kept.
    const int keep = std::max(1, (int)((l.verts + kVertexBudget - 1) / kVertexBudget));
    std::vector<float> pos;
    pos.reserve((size_t)(l.verts / keep + 1) * 3);
    for (uint32_t v = 0; v < l.verts; v += keep) {
        float p[3];
        std::memcpy(p, raw.data() + (size_t)v * l.stride, sizeof p);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])
            || std::fabs(p[0]) > 1e4f || std::fabs(p[1]) > 1e4f || std::fabs(p[2]) > 1e4f) {
            why = "the vertex buffer holds something that is not positions"; return nullptr;
        }
        pos.insert(pos.end(), p, p + 3);
    }

    const uintptr_t invAt = mem.readPtr(asset + g_off.SkeletalMesh_RefBasesInvMatrix);
    const int32_t invNum = mem.read<int32_t>(asset + g_off.SkeletalMesh_RefBasesInvMatrix + 8);
    if (!invAt || invNum != rig.count) {
        char b[128];
        snprintf(b, sizeof b, "the asset carries %d bone poses and the skeleton has %d bones",
                 invNum, rig.count);
        why = b; return nullptr;
    }
    std::vector<float> mats((size_t)invNum * 16);
    if (!mem.read_raw(invAt, mats.data(), mats.size() * sizeof(float))) {
        why = "the bone poses could not be read"; return nullptr;
    }

    // A level made of one section names the bones that carry it.
    std::vector<uint8_t> allow;
    const uintptr_t secAt = mem.readPtr(l.at + g_off.LOD_RenderSections);
    const int32_t secNum = mem.read<int32_t>(l.at + g_off.LOD_RenderSections + 8);
    if (secAt && secNum == 1 && g_off.Section_BoneMap) {
        const uintptr_t mapAt = mem.readPtr(secAt + g_off.Section_BoneMap);
        const int32_t mapNum = mem.read<int32_t>(secAt + g_off.Section_BoneMap + 8);
        const uint32_t secVerts = mem.read<uint32_t>(secAt + g_off.Section_NumVertices);
        const uint32_t secBase  = mem.read<uint32_t>(secAt + g_off.Section_BaseVertexIndex);
        if (mapAt && mapNum > 0 && mapNum <= rig.count && secBase == 0 && secVerts == l.verts) {
            std::vector<uint16_t> map((size_t)mapNum);
            if (mem.read_raw(mapAt, map.data(), map.size() * sizeof(uint16_t))) {
                allow.assign(rig.count, 0);
                bool sane = true;
                for (uint16_t b : map) { if (b >= rig.count) sane = false; else allow[b] = 1; }
                if (!sane) allow.clear();
            }
        }
    }

    if (!assemble(*m, pos, mats, rig, allow, why)) return nullptr;
    return m;
}

inline void workerLoop() {
    Builder& B = builder();
    for (;;) {
        Builder::Job job;
        {
            std::unique_lock<std::mutex> lk(B.mtx);
            B.cv.wait(lk, [&] { return B.stop || !B.jobs.empty(); });
            if (B.stop) return;
            job = B.jobs.front();
            B.jobs.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::string why;
        std::shared_ptr<const Model> m = read(*job.mem, job.asset, *job.rig, why);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(B.mtx);
        if (m) {
            B.ready[job.asset] = m;
            B.built++;
            printf("[body] mesh 0x%lx: level %d of %d, %d vertices %.1f cm apart, "
                   "%d bones, %.0f cm tall%s%s (%.0f ms)\n",
                   (unsigned long)job.asset, m->lod, m->lods, m->verts, m->spacing,
                   m->bones, m->height, m->classified ? "" : ", parts unknown",
                   m->armsDown ? ", modelled with its arms against its body, so where "
                                 "an arm touches the body the outline may smear" : "",
                   ms);
        } else {
            B.failed[job.asset] = job.mem->readPtr(job.asset + g_off.SkeletalMesh_RenderData);
            B.refused++;
            printf("[body] mesh 0x%lx: %s -- the outline skips it and the trigger "
                   "uses the tubes\n", (unsigned long)job.asset, why.c_str());
        }
        fflush(stdout);
    }
}

inline void shutdown() {
    Builder& B = builder();
    {
        std::lock_guard<std::mutex> lk(B.mtx);
        B.stop = true;
    }
    B.cv.notify_all();
    if (B.worker.joinable()) B.worker.join();
}

inline bool haveOffsets() {
    return g_off.Mesh_SkeletalMeshAsset && g_off.SkeletalMesh_RenderData
        && g_off.LOD_PositionBuffer && g_off.PosBuffer_Data && g_off.PosBuffer_Stride
        && g_off.PosBuffer_NumVertices && g_off.SkeletalMesh_RefBasesInvMatrix;
}

// The model behind a mesh component, or nothing while it is still being read.
// An asset is read once; it is read again only if what it points at changes,
// which is a different asset having taken its address.
inline std::shared_ptr<const Model> modelFor(const Mem& mem, uintptr_t mesh,
                                             const skel::Rig* rig) {
    if (!rig || !haveOffsets()) return nullptr;
    const uintptr_t asset = mem.readPtr(mesh + g_off.Mesh_SkeletalMeshAsset);
    if (!asset || (asset & 7)) return nullptr;
    Builder& B = builder();
    std::lock_guard<std::mutex> lk(B.mtx);
    auto it = B.ready.find(asset);
    if (it != B.ready.end()) {
        if (mem.readPtr(asset + g_off.SkeletalMesh_RenderData) == it->second->renderData)
            return it->second;
        B.ready.erase(it);                        // someone else lives here now
    }
    auto f = B.failed.find(asset);
    if (f != B.failed.end()) {
        if (mem.readPtr(asset + g_off.SkeletalMesh_RenderData) == f->second) return nullptr;
        B.failed.erase(f);
    }
    for (const auto& j : B.jobs) if (j.asset == asset) return nullptr;
    if (B.ready.size() + B.failed.size() > 128) { B.ready.clear(); B.failed.clear(); }
    B.jobs.push_back({ &mem, asset, rig });
    if (!B.worker.joinable()) B.worker = std::thread(workerLoop);
    B.cv.notify_one();
    return nullptr;
}

// ── handing the bodies to the overlay ────────────────────────────────────────

inline std::shared_ptr<const Frame> g_shown;       // std::atomic_load/store only
inline std::shared_ptr<Frame> g_slot[2];           // the reader's alone
inline Frame* g_filling = nullptr;
inline int    g_fillingSlot = -1;

inline std::shared_ptr<const Frame> current() { return std::atomic_load(&g_shown); }

// Two frames take turns, so a body's vertices are written into storage the
// overlay is not reading. If the overlay still holds both, a third is made
// rather than wait for it.
inline void beginFrame() {
    const std::shared_ptr<const Frame> shown = current();
    for (int i = 0; i < 2; i++) {
        if (!g_slot[i]) g_slot[i] = std::make_shared<Frame>();
        if (g_slot[i].get() == shown.get()) continue;
        if (g_slot[i].use_count() == 1) {
            g_slot[i]->n = 0;
            g_filling = g_slot[i].get();
            g_fillingSlot = i;
            return;
        }
    }
    const int i = (g_slot[0].get() == shown.get()) ? 1 : 0;
    g_slot[i] = std::make_shared<Frame>();
    g_filling = g_slot[i].get();
    g_fillingSlot = i;
}

inline void publish() {
    if (g_fillingSlot < 0) return;
    std::atomic_store(&g_shown, std::shared_ptr<const Frame>(g_slot[g_fillingSlot]));
    g_filling = nullptr;
    g_fillingSlot = -1;
}

// Called by the reader for each player whose bones it has just read and
// composed; `R` and `P` are those bones in component space.
inline bool add(const Mem& mem, uintptr_t mesh, const skel::Rig& rig,
                const std::vector<FQuat>& R, const std::vector<FVector>& P,
                int num, const FQuat& cq, const FVector& ct, const FVector& cs,
                uintptr_t id) {
    if (!g_filling) return false;
    std::shared_ptr<const Model> m = modelFor(mem, mesh, &rig);
    if (!m) return false;
    Frame& f = *g_filling;
    if (f.n >= (int)f.bodies.size()) f.bodies.emplace_back();
    Body& b = f.bodies[f.n];
    if (!pose(*m, R, P, num, cq, ct, cs, b.xyz, b.lo, b.hi)) return false;
    b.id = id;
    b.model = std::move(m);
    f.n++;
    return true;
}

}  // namespace body
