#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Whether a player can actually be seen, and therefore shot.
//
// The engine already works this out every frame: it decides what to draw, and
// it stamps each mesh with the time it last drew it. Reading that stamp is a
// better answer than anything a tool can compute from outside, because it is
// the same answer the game itself acted on, occlusion, culling, destroyed walls
// and all. What it takes to use it is getting two things right.
//
// WHICH STAMP. A mesh is drawn into more than the view on your screen: shadow
// maps, reflections and other passes all count as "rendered". The engine keeps
// a second stamp beside the first that is only touched when the mesh is drawn
// on screen, and that is the one worth reading. Which of the neighbouring
// fields it is differs by build, so it is not assumed: both are read, and the
// one that is never ahead of the other, and falls behind it at least sometimes,
// is the on-screen one. That is what being a subset means, written as a test.
//
// WHAT TIME IT IS NOW. A stamp is only meaningful against the engine's own
// clock, and that clock is not the wall clock: it pauses, it is dilated, and it
// restarts between rounds. The renderer stamps a mesh with the world's own
// TimeSeconds for the frame it drew, so where the world can be read, that is
// what "now" is and nothing is estimated. Where it cannot, the newest stamp any
// player carries stands in for it, carried forward by the wall clock between
// samples: close enough while anyone is on screen, and drifting while nobody
// is, which is the moment the answer matters most.
//
// HOW MUCH SLACK. The render thread writes a stamp up to a frame after the
// world's clock has moved on, and this reader samples at a rate of its own, so
// a player drawn a moment ago reads a frame or two old. Two of the game's own
// frames is the least that can be asked for without calling people on screen
// hidden.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "structs.hpp"

namespace vis {

// The floats around the derived one, read together. Which is which is decided
// by how they behave, not by their order.
inline constexpr int kNeighbours = 5;
inline constexpr int kOffsets[kNeighbours] = {-8, -4, 0, 4, 8};

struct Sample {
    float v[kNeighbours] = {0, 0, 0, 0, 0};
};

// Decides which neighbour is the on-screen stamp, from the one rule that has to
// hold: a mesh cannot be drawn on screen at a time it was not drawn at all, so
// the on-screen stamp is never ahead of the other one. A field that is always
// equal is the same stamp read twice, which tells us nothing; a field that is
// sometimes behind is the one we want.
class FieldPick {
public:
    // The index into kOffsets that is believed to hold the on-screen stamp,
    // or 2 (the derived offset itself) while nothing better has shown itself.
    int index() const { return chosen_; }
    int offset() const { return kOffsets[chosen_]; }
    bool decided() const { return decided_; }
    int  offScreenSeen() const { return far_[chosen_]; }

    void observe(const Sample& s) {
        const float all = s.v[2];
        if (!plausible(all)) return;
        for (int i = 0; i < kNeighbours; i++) {
            if (i == 2) continue;
            const float v = s.v[i];
            // A mesh never drawn on screen keeps whatever the field started
            // as, which is not a time at all. That is not a reason to throw
            // the field out: it is the very case being looked for, so it
            // counts as being a long way behind.
            if (!plausible(v)) { far_[i]++; continue; }
            // Ahead of the stamp we already have cannot be an on-screen stamp.
            if (v > all + 0.001f) { ahead_[i]++; continue; }
            // The signature is both of these, from the same field: level with
            // the other stamp for a player being drawn on screen, and a long
            // way behind it for one that is only in a shadow map. A field that
            // merely lags, the way a submission time does, is level with it
            // never; a field that is frozen is level with it never either.
            if (all - v < 0.020f) level_[i]++;
            else if (all - v > 0.500f) far_[i]++;
        }
        if (++seen_ < 240) return;                 // a few seconds of watching

        int best = 2, bestFar = 0;
        for (int i = 0; i < kNeighbours; i++) {
            if (i == 2 || ahead_[i] > 2) continue;             // not a subset
            if (level_[i] < 20 || far_[i] < 20) continue;      // not the signature
            if (far_[i] > bestFar) { bestFar = far_[i]; best = i; }
        }
        if (best != chosen_) chosen_ = best;
        decided_ = true;
    }

    void reset() {
        for (int i = 0; i < kNeighbours; i++) ahead_[i] = far_[i] = level_[i] = 0;
        seen_ = 0;
        chosen_ = 2;
        decided_ = false;
    }

private:
    // A stamp is a time since the level began, and an unrendered mesh carries a
    // large negative default rather than zero.
    static bool plausible(float v) {
        return std::isfinite(v) && v > 0.f && v < 1e7f;
    }

    int ahead_[kNeighbours] = {0, 0, 0, 0, 0};   // disqualifying
    int far_[kNeighbours]   = {0, 0, 0, 0, 0};   // a long way behind: off screen
    int level_[kNeighbours] = {0, 0, 0, 0, 0};   // level with it: on screen
    int seen_ = 0;
    int chosen_ = 2;
    bool decided_ = false;
};

// The engine's clock, anchored on the stamps themselves.
class Clock {
public:
    // `newest` is the newest stamp seen this frame, from any mesh, or zero when
    // nothing was rendered. `wall` is any steady clock, in seconds.
    void feed(float newest, double wall) {
        if (newest > 0.f) {
            // A big step backwards is the level clock restarting, which has to
            // be taken rather than ignored, or everything reads hidden forever.
            if (newest > anchor_ || newest < anchor_ - 5.f) {
                // How far the stamp jumps when it jumps is the game's own frame,
                // measured rather than assumed. It is the floor under any
                // tolerance: a stamp written one frame ago is not a player who
                // has gone anywhere, and asking for less slack than that calls
                // half the people on screen hidden.
                const float step = newest - anchor_;
                if (have_ && step > 0.001f && step < 0.5f) {
                    steps_[stepAt_++ % kSteps] = step;
                    if (stepN_ < kSteps) stepN_++;
                }
                anchor_ = newest;
                anchorWall_ = wall;
                have_ = true;
            }
        }
        wall_ = wall;
    }

    // The middle of the recent jumps, so one long hitch does not set the floor.
    float frame() const {
        if (stepN_ == 0) return 0.f;
        float v[kSteps];
        for (int i = 0; i < stepN_; i++) v[i] = steps_[i];
        for (int i = 1; i < stepN_; i++)
            for (int j = i; j > 0 && v[j] < v[j-1]; j--) std::swap(v[j], v[j-1]);
        return v[stepN_ / 2];
    }

    // The world's own time, read from the world itself. `t` is negative when
    // it could not be read, and `frame` is zero when the world's frame time did
    // not come with it. When it is good it replaces everything above: the
    // stamps are written from this very clock, so there is nothing to estimate.
    void feedEngine(uintptr_t world, double t, float frame, float newest) {
        engineOk_ = false;
        if (world != world_) {                 // a new level, a new world
            world_ = world; ahead_ = 0; distrust_ = false; told_ = false;
        }
        if (t <= 0.0 || distrust_) return;
        // The two have to agree in both directions. Nothing can have been drawn
        // later than the world's clock reads, past the moment between reading
        // the one and the other; and while the stamps are still climbing,
        // something was drawn a moment ago, so the clock cannot be far past the
        // newest of them. A clock that fails either is not the one the stamps
        // were written from, and a wrong clock is worse than the estimate, so it
        // is dropped until the next world.
        const bool live   = have_ && (wall_ - anchorWall_) < 0.25;
        const bool ahead  = newest > 0.f && (double)newest > t + 0.25;
        const bool behind = live && t - (double)anchor_ > 1.0;
        if (ahead || behind) {
            if (++ahead_ >= 30) distrust_ = true;
            return;
        }
        ahead_ = 0;
        engineNow_ = t;
        engineFrame_ = frame > 0.f ? frame : 0.f;
        engineOk_ = true;
    }

    bool have() const { return engineOk_ || have_; }
    bool engine() const { return engineOk_; }

    // True once, the first time the world's clock is dropped, so the caller
    // can say why.
    bool distrustedNews() {
        if (distrust_ && !told_) { told_ = true; return true; }
        return false;
    }

    // Now, on the engine's clock. Read from the world when it can be; between
    // anchors otherwise, running on the wall clock, which is right while the
    // game is running at all and wrong while it is paused or loading.
    double now() const {
        return engineOk_ ? engineNow_ : anchor_ + (wall_ - anchorWall_);
    }

    // The world's own frame time when it came with the clock, and the middle
    // of the recent stamp jumps when it did not. The second is never shorter
    // than the gap between this reader's samples, which is the right side to
    // err on for a floor.
    float frameNow() const {
        return (engineOk_ && engineFrame_ > 0.f) ? engineFrame_ : frame();
    }

    void reset() {
        have_ = false; anchor_ = 0.f; anchorWall_ = 0.0; wall_ = 0.0;
        stepN_ = 0; stepAt_ = 0;
        engineOk_ = false; world_ = 0; ahead_ = 0; distrust_ = false; told_ = false;
    }

private:
    uintptr_t world_ = 0;
    double engineNow_ = 0.0;
    float  engineFrame_ = 0.f;
    bool   engineOk_ = false;
    int    ahead_ = 0;
    bool   distrust_ = false;
    bool   told_ = false;

    static constexpr int kSteps = 16;
    float steps_[kSteps] = {0};
    int   stepAt_ = 0, stepN_ = 0;
    float  anchor_ = 0.f;
    double anchorWall_ = 0.0;
    double wall_ = 0.0;
    bool   have_ = false;
};

// Whether a body can be on your screen at all. A stamp says a body was drawn,
// and a body is drawn for more than your view: for the shadows it casts, and
// for any other view the game renders. One wholly outside your view can only
// have been drawn for one of those. The body is a sphere, tested against the
// four sides of the view as planes, so anyone even partly in counts as in.
// `cam` is the camera basis the ESP projects with: forward, right and up in its
// first three rows, the eye in the fourth. `fovDeg` is vertical, as this game's
// is, and `aspect` widens it to the sides.
class View {
public:
    View(const FMatrix& cam, double fovDeg, double aspect) : cam_(cam) {
        const double v = std::clamp(fovDeg, 1.0, 170.0) * 3.14159265358979323846 / 360.0;
        const double tanV = std::tan(v), tanH = tanV * (aspect > 0.1 ? aspect : 16.0 / 9.0);
        cosV_ = 1.0 / std::sqrt(1.0 + tanV * tanV); sinV_ = tanV * cosV_;
        cosH_ = 1.0 / std::sqrt(1.0 + tanH * tanH); sinH_ = tanH * cosH_;
    }
    bool holds(const FVector& centre, double radius) const {
        const double dx = centre.X - cam_.m[3][0];
        const double dy = centre.Y - cam_.m[3][1];
        const double dz = centre.Z - cam_.m[3][2];
        const double depth = dx * cam_.m[0][0] + dy * cam_.m[0][1] + dz * cam_.m[0][2];
        if (depth < -radius) return false;
        const double right = dx * cam_.m[1][0] + dy * cam_.m[1][1] + dz * cam_.m[1][2];
        const double up    = dx * cam_.m[2][0] + dy * cam_.m[2][1] + dz * cam_.m[2][2];
        // how far outside each side's plane the centre lies, against the radius
        if (std::fabs(right) * cosH_ - depth * sinH_ > radius) return false;
        if (std::fabs(up)    * cosV_ - depth * sinV_ > radius) return false;
        return true;
    }
private:
    FMatrix cam_;
    double cosV_ = 1, sinV_ = 0, cosH_ = 1, sinH_ = 0;
};

// Two frames of disagreement before the verdict changes. The stamp is a frame
// or two behind the game by the time it is read, so a player crossing a doorway
// sits on the boundary for a moment; without this the answer chatters, and
// everything downstream chases it.
class Verdict {
public:
    bool update(bool rawVisible) {
        if (rawVisible == state_) { run_ = 0; return state_; }
        if (++run_ >= 2) { state_ = rawVisible; run_ = 0; }
        return state_;
    }
    bool state() const { return state_; }
    void reset(bool v) { state_ = v; run_ = 0; }

private:
    bool state_ = true;
    int  run_ = 0;
};

}  // namespace vis
