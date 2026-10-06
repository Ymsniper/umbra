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
// WHAT TIME IT IS NOW. A stamp is only meaningful against the time of the
// latest frame drawn, and the newest stamp being written at the moment is
// exactly that whenever anyone is being drawn: someone drawn in that frame
// reads no age at all. Only stamps seen moving on count for it (Liveness),
// never one that merely reads newest. Between them it is carried forward by
// the wall clock, which runs with the game's while the game is running. The world's own TimeSeconds is
// not that time. It is the game thread's, and the render thread stamps the
// frame it draws a few frames later, or the clock read is not the one the
// stamps came from at all; either way a player drawn this very frame reads
// tens of milliseconds old against it, and a tolerance of a frame or two then
// calls everyone on screen hidden. So the world's clock gives the game's frame
// time, for the slack below, and stands in for the stamps only before there
// are any.
//
// HOW MUCH SLACK. This reader samples at a rate of its own, and a frame is
// drawn between one stamp and the next, so a player drawn a moment ago can
// read a frame or so old. Two of the game's own frames is the least that can
// be asked for without calling people on screen hidden.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
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

// Which stamps are being written right now. A stamp that moved on since it was
// last read, by no more than the time that has passed since, belongs to a mesh
// the game is drawing at this moment, so it is the time of the latest frame
// drawn. One that stands still says nothing about now, whatever it reads; and
// one that leapt further than time allows is not a stamp at all. A player being
// killed and torn down can leave anything in those fields for a while, and the
// newest of all the stamps, taken as now, then put every real stamp in the
// past for good: everyone read hidden until the next level.
//
// Everything drawn in one frame carries that frame's time, so stamps being
// written at the same moment agree to within a frame or two. One that runs
// well ahead of the others moving with it is not being written by the game
// either, whatever memory it is now; it is left out of the clock, and that
// player is not believed for a while.
class Liveness {
public:
    // Once a pass, before the stamps.
    void begin(double wall) { wall_ = wall; cand_.clear(); }

    // True when `stamp`, for whatever `key` names, is one being written now.
    bool advance(uintptr_t key, float stamp) {
        if (!(stamp > 0.f)) return false;
        Rec& r = recs_[key];
        bool live = false;
        if (r.seen && stamp > r.stamp) {
            const double dt = wall_ - r.wall;         // since it last moved
            live = dt > 0.0 && (double)(stamp - r.stamp) <= dt + 0.25;
        }
        if (!r.seen || stamp != r.stamp) { r.stamp = stamp; r.wall = wall_; r.seen = true; }
        if (live && trusted(key)) cand_.push_back({ key, stamp });
        return live;
    }

    // After the stamps: the newest of those being written now that agrees with
    // the rest, or zero when none is.
    float newest() {
        std::sort(cand_.begin(), cand_.end(), [](const Cand& a, const Cand& b) { return a.stamp > b.stamp; });
        size_t first = 0;
        while (first + 1 < cand_.size() && cand_[first].stamp > cand_[first + 1].stamp + kAgree) {
            recs_[cand_[first].key].distrustUntil = wall_ + kDistrustFor;
            dropped_++;
            first++;
        }
        if (recs_.size() > 512) recs_.clear();
        return first < cand_.size() ? cand_[first].stamp : 0.f;
    }

    // Not caught running ahead of the rest lately.
    bool trusted(uintptr_t key) const {
        auto it = recs_.find(key);
        return it == recs_.end() || wall_ >= it->second.distrustUntil;
    }
    // How many times a stamp has been left out for running ahead.
    int dropped() const { return dropped_; }
    void reset() { recs_.clear(); cand_.clear(); }

private:
    static constexpr float  kAgree = 0.25f;        // s: frames apart, never more
    static constexpr double kDistrustFor = 10.0;   // s
    struct Rec { float stamp = 0.f; double wall = 0.0; bool seen = false; double distrustUntil = 0.0; };
    struct Cand { uintptr_t key; float stamp; };
    std::unordered_map<uintptr_t, Rec> recs_;
    std::vector<Cand> cand_;
    double wall_ = 0.0;
    int dropped_ = 0;
};

// The engine's clock, anchored on the stamps themselves.
class Clock {
public:
    // `live` is the newest of the stamps being written now (see Liveness), or
    // zero when none is. `wall` is any steady clock, in seconds. It is taken
    // as it comes, earlier than before or not: a stamp being written now is
    // the time of the latest frame drawn, so whatever the anchor held before,
    // this is now, and an anchor that something false pushed ahead is put right
    // by the next player drawn. A new level's clock, restarted from nothing, is
    // taken the same way.
    void feed(float live, double wall) {
        if (live > 0.f) {
            // How far it moves on between frames is the game's own frame,
            // measured rather than assumed. It is the floor under any
            // tolerance: a stamp written one frame ago is not a player who has
            // gone anywhere, and asking for less slack than that calls half the
            // people on screen hidden.
            const float step = live - anchor_;
            if (have_ && step > 0.001f && step < 0.5f) {
                steps_[stepAt_++ % kSteps] = step;
                if (stepN_ < kSteps) stepN_++;
            }
            anchor_ = live;
            anchorWall_ = wall;
            have_ = true;
        }
        wall_ = wall;
    }

    // How long since a stamp was last seen being written, which is how long the
    // clock has been running on the wall clock alone.
    double sinceLive() const { return have_ ? wall_ - anchorWall_ : -1.0; }

    // Now as it stood before this pass's stamps: the anchor carried to `wall`.
    double at(double wall) const { return have_ ? anchor_ + (wall - anchorWall_) : 0.0; }

    // The middle of the recent jumps, so one long hitch does not set the floor.
    float frame() const {
        if (stepN_ == 0) return 0.f;
        float v[kSteps] = {0};
        for (int i = 0; i < stepN_; i++) v[i] = steps_[i];
        for (int i = 1; i < stepN_; i++)
            for (int j = i; j > 0 && v[j] < v[j-1]; j--) std::swap(v[j], v[j-1]);
        return v[stepN_ / 2];
    }

    // The world's own time, read from the world itself. `t` is negative when
    // it could not be read, and `frame` is zero when the world's frame time did
    // not come with it. It is trusted for the game's frame time, and stands in
    // for the stamps only before there are any; see WHAT TIME IT IS NOW.
    // `newest` is the newest stamp being written now, as fed above.
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

    // Now, on the stamps' own clock: the newest stamp, carried forward by the
    // wall clock since it last moved. The world's clock only before any stamp.
    double now() const {
        if (have_) return anchor_ + (wall_ - anchorWall_);
        return engineOk_ ? engineNow_ : 0.0;
    }

    // How far the world's clock runs ahead of the stamps' while both are
    // live, for the probe: the gap that judging by the world's clock used to
    // count against everyone. Negative when it cannot be said.
    double worldAhead() const {
        if (!engineOk_ || !have_ || wall_ - anchorWall_ > 0.25) return -1.0;
        return engineNow_ - (anchor_ + (wall_ - anchorWall_));
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
