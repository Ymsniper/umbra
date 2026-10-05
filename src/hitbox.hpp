#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Where a shot goes: a ray from the eye against the tubes a body is made of.
//
// A crosshair is a direction, not a dot. The game answers "did that hit" by
// tracing a line from the camera and asking what it ran into. Measuring pixels
// between the crosshair and a projected joint answers a different question, and
// the two disagree in exactly the cases that matter: a circle of pixels around
// an elbow covers empty air on one side and stops short of the forearm on the
// other, and the same circle is most of a torso at fifty metres and half a head
// at five. Worse, a joint is a point, so a limb between two joints was only ever
// covered at its ends. So the test here is the one the game runs.
//
// The tubes come from the rig that is already read for the skeleton. Their radii
// are not in it, and the game's own collision shapes need offsets that are not
// derived yet, so they are proportioned from the one measurement the pose always
// carries: the length from the hips to the head. Every part of a person scales
// with it, it barely changes when one crouches, and it is read live, so a bigger
// body gets bigger tubes without anything having to know which class it is.
//
// Which tube is which is read off the parent table, not typed in. The spine is
// the walk from the head up to the hips; whatever hangs off the hips is a leg
// and whatever hangs off the spine above them is an arm, and how far out along
// that branch a bone sits is what makes an upper arm thicker than a forearm.
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
#include "global.hpp"
#include "skeleton.hpp"

namespace hit {

// What the segment ENDING at a bone belongs to. The segment, not the joint: a
// tube runs from a joint to its parent, so the elbow is what carries the arm.
enum Class : uint8_t { cNone = 0, cHead, cNeck, cTorso, cArm, cLeg };

// Body parts as the menu offers them.
enum Part { pHead = 0, pChest = 1, pBody = 2, pLegs = 3, pAll = 4 };

struct Capsule {
    FVector a{0, 0, 0}, b{0, 0, 0};
    double  r = 0.0;
};

struct Ray {
    FVector o{0, 0, 0};
    FVector d{1, 0, 0};       // unit
    double  pxAt1 = 0.0;      // world units one pixel covers per unit of range
    bool    ok = false;
};

struct Result {
    bool   hit   = false;
    double range = 0.0;       // along the ray, world units
    double gap   = 0.0;       // world units short of the surface, 0 when inside
    double gapPx = 0.0;       // the same gap on screen, for the readout
};

// The ray through one pixel. The projection the ESP draws with keeps the camera
// axes as its first three rows and the camera position in the fourth, so this is
// exact rather than fitted: it is the same basis, read the other way round.
inline Ray rayThrough(const FMatrix& vp, float px, float py, int sw, int sh) {
    Ray r;
    float fov = vp.m[3][3] * g_fovScale;
    if (fov < 1.f || fov > 170.f) fov = 90.f * g_fovScale;
    // Half HEIGHT for both axes, as in worldToScreen: this game's FOV is vertical.
    const double scale = (sh * 0.5) / tan(fov * 3.14159265358979 / 360.0);
    if (!(scale > 1.0)) return r;
    const double sx =  (px - sw * 0.5) / scale;
    const double sy = -(py - sh * 0.5) / scale;   // screen Y grows downward
    r.o = FVector(vp.m[3][0], vp.m[3][1], vp.m[3][2]);
    FVector d(vp.m[0][0] + sx * vp.m[1][0] + sy * vp.m[2][0],
              vp.m[0][1] + sx * vp.m[1][1] + sy * vp.m[2][1],
              vp.m[0][2] + sx * vp.m[1][2] + sy * vp.m[2][2]);
    const double len = d.length();
    if (len < 1e-9) return r;
    r.d = d * (1.0 / len);
    r.pxAt1 = 1.0 / scale;
    r.ok = true;
    return r;
}

// Closest approach between the half-line and the segment [a,b], with the range
// along the ray where it happens. Clamped in both parameters, so a ray passing
// beside the end of a limb measures to the end of it and not to its axis
// extended, which is what gives a capsule its rounded caps.
inline double raySeg(const Ray& ray, const FVector& a, const FVector& b,
                     double& tRay) {
    const FVector v = b - a;
    const FVector w = ray.o - a;
    const double B = ray.d.X*v.X + ray.d.Y*v.Y + ray.d.Z*v.Z;
    const double C = v.X*v.X + v.Y*v.Y + v.Z*v.Z;
    const double D = ray.d.X*w.X + ray.d.Y*w.Y + ray.d.Z*w.Z;
    const double E = v.X*w.X + v.Y*w.Y + v.Z*w.Z;
    const double den = C - B * B;                  // the ray is already unit
    double t;                                      // along the segment
    if (den > 1e-9 && C > 1e-9) t = (E - B * D) / den;
    else if (C > 1e-9)          t = E / C;          // parallel: anywhere will do
    else                        t = 0.0;            // a point, not a segment
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    double s = t * B - D;                           // back onto the ray
    if (s < 0.0) {                                  // behind the eye
        s = 0.0;
        if (C > 1e-9) { t = E / C; if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0; }
    }
    tRay = s;
    const FVector p(ray.o.X + ray.d.X*s - (a.X + v.X*t),
                    ray.o.Y + ray.d.Y*s - (a.Y + v.Y*t),
                    ray.o.Z + ray.d.Z*s - (a.Z + v.Z*t));
    return p.length();
}

// The rig, read once for what each bone is part of.
struct Skin {
    bool ok = false;
    int  count = 0;
    std::vector<uint8_t> cls;      // Class of the segment ending at this bone
    std::vector<uint8_t> step;     // steps out from the spine; 0 on it
    std::vector<int16_t> spineAt;  // place along the spine, -1 if off it
    int head = -1, neck = -1, pelvis = -1;
    int chestAt = 0;               // where the chest sits along the spine
    int spineLen = 0;
};

// Limbs past the forearm and the ankle are left out on purpose. A hand is a
// fraction of a silhouette, but a held weapon is parented to it, and so are the
// aim helpers some rigs carry, and neither is a thing that can be shot.
inline constexpr int kMaxStep = 4;

inline const Skin* skinFor(const skel::Rig* rg) {
    static std::map<const skel::Rig*, Skin> cache;
    // The overlay asks every frame and the mesh reader asks from a thread of
    // its own. An entry is written once and never moved, so what is handed out
    // stays good after the lock is let go.
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    if (!rg || !rg->ok) return nullptr;
    auto it = cache.find(rg);
    if (it != cache.end()) return it->second.ok ? &it->second : nullptr;

    Skin s;
    const std::vector<int32_t>& par = rg->parents;
    const int n = rg->count;
    s.count = n;
    if (n < 4 || (int)par.size() < n || rg->head < 1 || rg->head >= n
        || rg->pelvis < 1 || rg->pelvis >= n) {
        cache[rg] = std::move(s);
        return nullptr;
    }

    // The spine: the walk from the head up to the hips. If the hips are not on
    // that walk the two are not the ends of one chain, and nothing here holds.
    std::vector<int> chain;
    for (int b = rg->head; b >= 0 && (int)chain.size() < 32; b = par[b]) {
        chain.push_back(b);
        if (b == rg->pelvis) break;
    }
    if (chain.size() < 2 || chain.back() != rg->pelvis) {
        cache[rg] = std::move(s);
        return nullptr;
    }
    for (size_t i = 0, j = chain.size() - 1; i < j; ++i, --j)
        std::swap(chain[i], chain[j]);            // hips first, head last

    std::vector<int> nkids(n, 0);
    for (int i = 1; i < n; ++i)
        if (par[i] >= 0 && par[i] < n) nkids[par[i]]++;

    s.cls.assign(n, cNone);
    s.step.assign(n, 0);
    s.spineAt.assign(n, -1);
    for (size_t k = 0; k < chain.size(); ++k) {
        s.spineAt[chain[k]] = (int16_t)k;
        s.cls[chain[k]] = cTorso;
    }
    s.head   = rg->head;
    s.pelvis = rg->pelvis;
    s.spineLen = (int)chain.size();
    s.cls[s.head] = cHead;
    const int neck = par[s.head];
    if (neck > 0 && s.spineAt[neck] > 0) { s.cls[neck] = cNeck; s.neck = neck; }

    // The chest: the fork on the spine that carries the most branches, which is
    // what the two collarbones make it. Never the neck or the head.
    s.chestAt = -1;
    int most = 1;
    for (int k = 1; k + 2 < (int)chain.size(); ++k) {
        if (nkids[chain[k]] > most) { most = nkids[chain[k]]; s.chestAt = k; }
    }
    if (s.chestAt < 0)
        s.chestAt = (int)chain.size() >= 4 ? (int)chain.size() - 3 : 1;

    // Everything else: walk up to the spine and see where it joins. What joins
    // at the hips is a leg, what joins above them is an arm. What never reaches
    // the spine at all hangs off the root, and is a helper rather than a limb.
    for (int i = 1; i < n; ++i) {
        if (s.spineAt[i] >= 0) continue;
        int up = i, steps = 0;
        while (up > 0 && s.spineAt[up] < 0 && steps < 24) { up = par[up]; ++steps; }
        if (up <= 0 || s.spineAt[up] < 0) continue;
        if (s.cls[up] == cHead) continue;     // hair and jaw: the skull covers them
        s.step[i] = (uint8_t)(steps < 31 ? steps : 31);
        s.cls[i]  = (s.spineAt[up] == 0) ? cLeg : cArm;
    }

    int nArm = 0, nLeg = 0, nOut = 0;
    for (int i = 1; i < n; ++i) {
        if (s.cls[i] == cArm) ++nArm;
        else if (s.cls[i] == cLeg) ++nLeg;
        else if (s.cls[i] == cNone) ++nOut;
    }
    // THE CONTROL: what came out has to be a person. A wrong head or hip index,
    // from a game update that moved them, gives a short spine or a body with no
    // limbs hanging off it, and that is worth falling back from rather than
    // shooting at. The collision capsule stands in when this fails.
    if (s.spineLen < 4 || nArm < 4 || nLeg < 4) {
        printf("[hit] rig of %d bones is not a person (spine %d, arms %d, "
               "legs %d) -- the trigger will use the collision capsule\n",
               n, s.spineLen, nArm, nLeg);
        cache[rg] = Skin{};
        return nullptr;
    }
    printf("[hit] rig of %d bones: spine %d (chest at %d), arms %d, legs %d, "
           "%d left out\n", n, s.spineLen, s.chestAt, nArm, nLeg, nOut);
    s.ok = true;
    cache[rg] = std::move(s);
    return &cache[rg];
}

// Radii, as fractions of the hip-to-head length. For a person of average build
// that length is about a third of their height, so the numbers below read as
// centimetres of body on a 180: a 15 cm half-width through the chest, 9 at the
// skull, 5 through the upper arm.
inline double radiusFrac(uint8_t cls, uint8_t step) {
    switch (cls) {
        case cHead:  return 0.150;
        case cNeck:  return 0.100;
        case cTorso: return 0.250;
        case cArm:   return step <= 1 ? 0.160    // inside the chest
                          : step == 2 ? 0.110    // the shoulder
                          : step == 3 ? 0.090    // the upper arm
                                      : 0.075;  // the forearm
        case cLeg:   return step <= 1 ? 0.140    // the hip
                          : step == 2 ? 0.120    // the thigh
                          : step == 3 ? 0.095    // the shin
                                      : 0.080;  // the foot
        default:     return 0.0;
    }
}

inline bool wanted(int part, uint8_t cls, int spineAt, int chestAt) {
    switch (part) {
        case pHead:  return cls == cHead;
        case pChest: return cls == cNeck || (cls == cTorso && spineAt >= chestAt);
        case pBody:  return cls == cTorso || cls == cNeck;
        case pLegs:  return cls == cLeg;
        default:     return cls != cNone;
    }
}

// The tubes for one player, in world space. Returns how many were written.
inline int capsules(const EntityData& e, int part, double sizeScale,
                    Capsule* out, int max) {
    const skel::Rig* rg = static_cast<const skel::Rig*>(e.rig);
    const Skin* sk = skinFor(rg);
    if (!sk || e.rigCount < 2 || max < 1) return 0;
    const int n = e.rigCount < sk->count ? e.rigCount : sk->count;
    if (sk->head >= n || sk->pelvis >= n) return 0;

    auto bone = [&](int i) -> const FVector& { return e.rigBones[i]; };
    const bool haveEnds = !bone(sk->head).isZero() && !bone(sk->pelvis).isZero();

    // The one live measurement everything else is proportioned from. Falling
    // back to the collision capsule keeps a body the right size when the pose
    // itself is half read, rather than making it a point.
    const double measured = haveEnds ? bone(sk->head).dist(bone(sk->pelvis)) : 0.0;

    // THE CONTROL, on the live pose: the two ends of that measurement have to be
    // a person's hips and head. The game's own eye height says how tall this
    // class is and, unlike the collision capsule, it does not shrink when they
    // crouch, so what the two make together holds in any pose. A head index left
    // over from before an update moved the bones lands somewhere else on the
    // body and fails this, and the collision capsule stands in instead.
    if (haveEnds && e.eyeHeight > 10.f) {
        const double ratio = measured / (double)e.eyeHeight;
        if (ratio < 0.55 || ratio > 1.35) {
            static bool said = false;
            if (!said) {
                said = true;
                printf("[hit] bone %d to bone %d measures %.0f cm against a "
                       "%.0f cm eye height, which is not hips to head -- the "
                       "trigger will use the collision capsule\n",
                       sk->pelvis, sk->head, measured, (double)e.eyeHeight);
            }
            return 0;
        }
    }

    double S = measured;
    if (S < 20.0 || S > 140.0)
        S = e.capsuleHalf > 20.f ? 2.0 * (double)e.capsuleHalf * 0.33 : 60.0;
    const double scale = sizeScale > 0.0 ? sizeScale : 0.0;

    // What the game says this body's width is, reported once per value seen. The
    // tubes are not scaled by it: whether it differs between the classes or is
    // one number for everyone decides whether it can be, and that is a
    // measurement out of a match rather than something to assume here.
    if (e.capsuleRadius > 10.f) {
        static std::map<int, int> seen;
        const int key = (int)(e.capsuleRadius + 0.5f);
        if (seen.find(key) == seen.end() && seen.size() < 16) {
            seen[key] = 1;
            printf("[hit] a body of %.0f cm hip to head carries a %.1f cm "
                   "collision radius\n", S, (double)e.capsuleRadius);
        }
    }

    int got = 0;
    // The skull. It is not a segment in the rig: the head joint sits at the base
    // of it, so the tube runs on up the way the head leaves the neck.
    if ((part == pHead || part == pAll) && !bone(sk->head).isZero()) {
        FVector up(0, 0, 1);
        if (sk->neck > 0 && sk->neck < n && !bone(sk->neck).isZero()) {
            const FVector v = bone(sk->head) - bone(sk->neck);
            const double l = v.length();
            if (l > 1e-6) up = v * (1.0 / l);
        }
        Capsule c;
        c.a = bone(sk->head) + up * (0.03 * S);
        c.b = bone(sk->head) + up * (0.11 * S);
        c.r = radiusFrac(cHead, 0) * S * scale;
        if (c.r > 0.0) out[got++] = c;
    }

    for (int i = 1; i < n && got < max; ++i) {
        const uint8_t cls = sk->cls[i];
        if (cls == cNone || cls == cHead) continue;     // the skull is built above
        // The tube ending at the hips comes up from the root bone, which stands
        // on the ground between the feet: that is not a part of anyone.
        if (sk->spineAt[i] == 0) continue;
        if ((cls == cArm || cls == cLeg) && sk->step[i] > kMaxStep) continue;
        if (!wanted(part, cls, sk->spineAt[i], sk->chestAt)) continue;
        const int p = rg->parents[i];
        if (p < 0 || p >= n) continue;
        if (bone(i).isZero() || bone(p).isZero()) continue;
        Capsule c;
        c.a = bone(p);
        c.b = bone(i);
        // Rigs carry sockets for gadgets and props, and a socket can be parented
        // to the spine or to a hand and then sit wherever the thing it holds is.
        // Nothing on a person is longer than its own height from the hips, so a
        // tube that is says it is not part of one.
        const double seg = c.a.dist(c.b);
        if (seg > 1.2 * S) continue;
        if (haveEnds) {
            const FVector mid((c.a.X + c.b.X) * 0.5, (c.a.Y + c.b.Y) * 0.5,
                              (c.a.Z + c.b.Z) * 0.5);
            if (mid.dist(bone(sk->pelvis)) > 2.0 * S) continue;
        }
        c.r = radiusFrac(cls, sk->step[i]) * S * scale;
        // A limb no longer than it is wide is a twist or roll helper riding
        // inside a real one, not a limb of its own; the real one covers it.
        if (cls == cArm || cls == cLeg) {
            const double cap = seg * 0.75;
            if (c.r > cap) c.r = cap;
        }
        if (c.r > 0.0) out[got++] = c;
    }
    return got;
}

// The body's collision capsule, for a player whose pose could not be read. It
// is the shape the game itself moves around, so it is the right fallback: too
// fat to be a hitbox, but never in the wrong place. When the game's own radius
// came through it is used as it stands, because there is nothing to improve on.
inline int bodyCapsule(const EntityData& e, double sizeScale, Capsule* out) {
    if (e.capsuleHalf <= 20.f) return 0;
    const double half = (double)e.capsuleHalf;
    const double own = e.capsuleRadius > 10.f ? (double)e.capsuleRadius
                                             : 0.18 * 2.0 * half;
    const double r = own * (sizeScale > 0.0 ? sizeScale : 0.0);
    if (r <= 0.0) return 0;
    const double inset = r < half ? r : half * 0.5;
    out[0].a = FVector(e.origin.X, e.origin.Y, e.origin.Z + half - inset);
    out[0].b = FVector(e.origin.X, e.origin.Y, e.origin.Z - half + inset);
    out[0].r = r;
    return 1;
}

// Does the ray run through this player? `padPx` widens every tube by that many
// pixels at its own range, so the forgiveness slider keeps meaning what it says
// on screen while the test itself stays in the world. `shift` moves the whole
// body along: a pose is read a frame or two before it is asked about, and a
// player running past at close range covers a hand's width in that time.
inline Result against(const EntityData& e, const Ray& ray, int part,
                      double padPx, double sizeScale,
                      const FVector& shift = FVector(0, 0, 0)) {
    Result res;
    if (!ray.ok) return res;
    Capsule cs[192];
    int n = capsules(e, part, sizeScale, cs, 192);
    if (n == 0) n = bodyCapsule(e, sizeScale, cs);
    if (n == 0) return res;
    if (!shift.isZero())
        for (int i = 0; i < n; ++i) { cs[i].a = cs[i].a + shift; cs[i].b = cs[i].b + shift; }

    double bestGap = 1e18, gapRange = 0.0;
    double hitRange = 1e18;
    for (int i = 0; i < n; ++i) {
        double t = 0.0;
        const double d = raySeg(ray, cs[i].a, cs[i].b, t);
        if (t <= 1.0) continue;                          // at or behind the eye
        const double pad = padPx > 0.0 ? padPx * t * ray.pxAt1 : 0.0;
        const double gap = d - (cs[i].r + pad);
        if (gap <= 0.0) {
            res.hit = true;
            if (t < hitRange) hitRange = t;
        } else if (gap < bestGap) {
            bestGap = gap; gapRange = t;
        }
    }
    if (res.hit) {
        res.range = hitRange;
        res.gap = 0.0;
        res.gapPx = 0.0;
    } else if (bestGap < 1e17) {
        res.range = gapRange;
        res.gap = bestGap;
        const double perPx = gapRange * ray.pxAt1;
        res.gapPx = perPx > 1e-9 ? bestGap / perPx : 0.0;
    }
    return res;
}

}  // namespace hit
