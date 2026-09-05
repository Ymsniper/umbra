#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Engine types (FVector, FRotator, FMatrix) and the world-to-screen
// projection the ESP draws with.
#include <cmath>
#include <cstring>

// UE5 math types  (doubles for FVector/FRotator as per this SDK)

struct FVector {
    double X = 0, Y = 0, Z = 0;

    FVector() = default;
    FVector(double x, double y, double z) : X(x), Y(y), Z(z) {}

    FVector operator-(const FVector& o) const { return {X-o.X, Y-o.Y, Z-o.Z}; }
    FVector operator+(const FVector& o) const { return {X+o.X, Y+o.Y, Z+o.Z}; }
    FVector operator*(double s)          const { return {X*s,   Y*s,   Z*s};   }

    double length() const { return sqrt(X*X + Y*Y + Z*Z); }
    double dist(const FVector& o) const { return (*this - o).length(); }
    bool   isZero()  const { return X == 0 && Y == 0 && Z == 0; }
};

struct FRotator {
    double Pitch = 0, Yaw = 0, Roll = 0;
};

struct FQuat {
    double X = 0, Y = 0, Z = 0, W = 1;
};

// FTransform as read from memory.
// UE5 double layout: FQuat(32) + FVector Translation(24) + FVector Scale3D(24) = 80 bytes.
// No SIMD padding on the Linux non-ISPC path.
struct FTransform {
    FQuat   Rotation;    // +0x00  (32 bytes)
    FVector Translation; // +0x20  (24 bytes)
    FVector Scale3D;     // +0x38  (24 bytes)
};
static_assert(sizeof(FTransform) == 80, "FTransform size mismatch");

// 4×4 Matrix  (column-major, matches UE projection matrix)
struct FMatrix {
    float m[4][4];

    FMatrix() { memset(m, 0, sizeof(m)); }

    // Matrix × Vector4 (homogeneous)
    void transform(float ix, float iy, float iz,
                   float& ox, float& oy, float& oz, float& ow) const {
        ox = ix*m[0][0] + iy*m[1][0] + iz*m[2][0] + m[3][0];
        oy = ix*m[0][1] + iy*m[1][1] + iz*m[2][1] + m[3][1];
        oz = ix*m[0][2] + iy*m[1][2] + iz*m[2][2] + m[3][2];
        ow = ix*m[0][3] + iy*m[1][3] + iz*m[2][3] + m[3][3];
    }
};

struct ViewInfo {
    FVector  Location;
    FRotator Rotation;
    float    FOV = 80.f;
};

// World-to-Screen projection
struct Vec2 { float x = 0, y = 0; };

// Projection fine-tune. Declared here (not global.hpp) because global.hpp
// includes this header - the reverse include is circular.
inline float g_fovScale = 1.00f;

inline FMatrix buildVPMatrix(const ViewInfo& vi, int sw, int sh) {
    (void)sw; (void)sh;
    const double D2R = 3.14159265358979323846 / 180.0;
    double pitch = vi.Rotation.Pitch * D2R;
    double yaw   = vi.Rotation.Yaw   * D2R;
    double roll  = vi.Rotation.Roll  * D2R;

    double sp = sin(pitch), cp = cos(pitch);
    double sy = sin(yaw),   cy = cos(yaw);
    double sr = sin(roll),  cr = cos(roll);

    FMatrix out;
    // forward (UE X axis)
    out.m[0][0] = (float)(cp * cy);
    out.m[0][1] = (float)(cp * sy);
    out.m[0][2] = (float)(sp);
    // right (UE Y axis)
    out.m[1][0] = (float)(sr * sp * cy - cr * sy);
    out.m[1][1] = (float)(sr * sp * sy + cr * cy);
    out.m[1][2] = (float)(-sr * cp);
    // up (UE Z axis)
    out.m[2][0] = (float)(-(cr * sp * cy + sr * sy));
    out.m[2][1] = (float)(-(cr * sp * sy - sr * cy));
    out.m[2][2] = (float)(cr * cp);
    // camera position + FOV
    out.m[3][0] = (float)vi.Location.X;
    out.m[3][1] = (float)vi.Location.Y;
    out.m[3][2] = (float)vi.Location.Z;
    out.m[3][3] = vi.FOV;
    return out;
}

inline bool worldToScreen(const FMatrix& vp,
                          const FVector& world,
                          Vec2& screen, int sw, int sh) {
    float dx = (float)(world.X - vp.m[3][0]);
    float dy = (float)(world.Y - vp.m[3][1]);
    float dz = (float)(world.Z - vp.m[3][2]);

    float depth = dx * vp.m[0][0] + dy * vp.m[0][1] + dz * vp.m[0][2];
    if (depth < 1.f) return false;                 // behind the camera

    float right = dx * vp.m[1][0] + dy * vp.m[1][1] + dz * vp.m[1][2];
    float up    = dx * vp.m[2][0] + dy * vp.m[2][1] + dz * vp.m[2][2];

    float fov = vp.m[3][3] * g_fovScale;
    if (fov < 1.f || fov > 170.f) fov = 90.f * g_fovScale;
    float cx = sw * 0.5f, cy = sh * 0.5f;
    // Numerator is half HEIGHT, not half width, for BOTH axes. This game's
    // FOV behaves as vertical, so using cx made the scale 960/522 = 1.837x too
    // large and every target drifted outward from centre, worsening with angle.
    float scale = cy / tanf(fov * 3.14159265f / 360.f);   // FOV/2 in radians

    screen.x = cx + right * scale / depth;
    screen.y = cy - up    * scale / depth;
    return true;
}

// Transform a component-space bone FTransform into world space.
inline FVector transformBone(const FTransform& ctw, const FTransform& bone) {
    // Quaternion rotation: q * v * q^-1
    // Using fast formula: v' = v + 2w(q×v) + 2(q×(q×v))
    const FQuat& q = ctw.Rotation;
    FVector v = bone.Translation;

    double t[3];
    // q × v  (cross product of q.xyz and v)
    t[0] = q.Y * v.Z - q.Z * v.Y;
    t[1] = q.Z * v.X - q.X * v.Z;
    t[2] = q.X * v.Y - q.Y * v.X;

    // v + 2w*t + 2*(q×t)
    double rx = v.X + 2.0*q.W*t[0] + 2.0*(q.Y*t[2] - q.Z*t[1]);
    double ry = v.Y + 2.0*q.W*t[1] + 2.0*(q.Z*t[0] - q.X*t[2]);
    double rz = v.Z + 2.0*q.W*t[2] + 2.0*(q.X*t[1] - q.Y*t[0]);

    // Scale then translate
    return {
        rx * ctw.Scale3D.X + ctw.Translation.X,
        ry * ctw.Scale3D.Y + ctw.Translation.Y,
        rz * ctw.Scale3D.Z + ctw.Translation.Z
    };
}
