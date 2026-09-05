#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Game offsets, read from offsets.cfg at startup. Every value
// is 0 here so a missing file fails loudly instead of silently.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <fstream>
#include <sstream>

// Runtime offsets
struct RuntimeOffsets {
    // ---- verified by a closed two-hop cycle -------------------------------
 // ---- the GEngine chain: every live object by pointer, no heap sweep -------
    uintptr_t GEngine_RVA                 = 0x0;   // static; NEVER FOUND on this build
    uintptr_t GObjects_RVA                = 0x0;   // module global holding it
    uintptr_t GObjects_Key                = 0x0;   // XOR after the shuffles
    uintptr_t GObjects_NumKey             = 0x0;   // NumElements XOR, bswapped
    uintptr_t GObjects_ObjKey             = 0x0;   // Objects XOR, bswapped
    uintptr_t GObjects_Imm1               = 0x0;   // first pshuflw immediate
    uintptr_t GObjects_Imm2               = 0x0;   // second pshuflw immediate
    uintptr_t GObjects_Shift              = 0x0;   // psrld count, per 32-bit lane
    uintptr_t GObjects_NumOff             = 0x0;   // NumElements member offset
    uintptr_t GObjects_ObjOff             = 0x0;   // Objects member offset
    uintptr_t UGameEngine_GameInstance    = 0x0;
    uintptr_t UGameInstance_LocalPlayers  = 0x0;
    uintptr_t UPlayer_PlayerController    = 0x0;
    uintptr_t ADiscoveryCharacter_AnimSU  = 0x0;
    uintptr_t AnimSU_GameState            = 0x0;

    uintptr_t UWorld_PersistentLevel      = 0x0;
    uintptr_t ULevel_OwningWorld          = 0x0;
    uintptr_t AController_Pawn            = 0x0;
    uintptr_t APawn_Controller            = 0x0;

    // ---- verified live ----------------------------------------------------
    uintptr_t UWorld_NetDriver            = 0x0;
    uintptr_t UWorld_Levels               = 0x0;
    uintptr_t AController_PlayerState     = 0x0;
    uintptr_t AController_ControlRotation = 0x0;   // FRotator, 3 doubles
    uintptr_t APawn_BaseEyeHeight         = 0x0;   // float
    uintptr_t AActor_RootComponent        = 0x0;
    uintptr_t USceneComponent_RelLocation = 0x0;   // FVector, 3 doubles
    uintptr_t AGameStateBase_PlayerArray  = 0x0;
    uintptr_t AGameStateBase_WorldTime    = 0x0;   // ReplicatedWorldTimeSecondsDouble
    uintptr_t APlayerState_PawnPrivate    = 0x0;
    uintptr_t APlayerState_Spectator      = 0x0;   // bitfield byte, bit 1
    uintptr_t APlayerState_DisplayName    = 0x0;   // FString
    uintptr_t APlayerState_ClubTag        = 0x0;   // FString, may be empty
    uintptr_t APlayerState_Discriminator  = 0x0;   // FString "#1234"
    uintptr_t APlayerCameraManager_PCOwner = 0x0;
    uintptr_t APlayerCameraManager_POVLoc = 0x0;   // FVector, POV.Location
    uintptr_t POV_RotOffset               = 0x0;   // FRotator, from POV.Location
    uintptr_t POV_FovOffset               = 0x0;   // float, from POV.Location

    // ---- vtable RVAs (module base + RVA == IDA address) --------------------
    uintptr_t VT_UWorld                   = 0x0;
    uintptr_t VT_ULevel                   = 0x0;
    uintptr_t VT_APlayerController        = 0x0;
    uintptr_t VT_APawn                    = 0x0;
    uintptr_t VT_APlayerState             = 0x0;
    uintptr_t VT_AGameStateBase           = 0x0;
    uintptr_t VT_APlayerCameraManager     = 0x0;

    // ---- skeleton (verified live) ----------------------------------------
    //  FTransform here is 96 BYTES (alignas(16) per member), not 80.
    uintptr_t APawn_Mesh                  = 0x0;   // USkeletalMeshComponent*
    uintptr_t Mesh_SkeletalMeshAsset      = 0x0;   // USkinnedMeshComponent::SkeletalMesh
    uintptr_t SkeletalMesh_Skeleton       = 0x0;   // USkeletalMesh::Skeleton
    // Derived by the update tool, not hunted for at startup. The ESP scanning
    // for these itself was the odd one out in this project -- everything else
    // is derived offline, verified, written down, and simply read here.
    uintptr_t Skeleton_ParentTable        = 0x0;   // TArray<FMeshBoneInfo>
    uintptr_t Skeleton_BindPose           = 0x0;   // TArray<FTransform>, reference pose
    uintptr_t Bone_Head                   = 0x0;   // head JOINT index (not an offset)
    uintptr_t Mesh_BoneTree               = 0x0;
    uintptr_t BoneTree_Parents            = 0x0;
    uintptr_t Bone_Pelvis                 = 0x0;   // pelvis index (not an offset)
    uintptr_t Mesh_BoneArray              = 0x0;   // TArray<FTransform>, bone-space
    uintptr_t Mesh_ComponentToWorld       = 0x0;   // FTransform
    uintptr_t Mesh_LastRenderTime         = 0x0;
    uintptr_t SceneComp_ComponentVelocity = 0x0;
    // ACharacter::CharacterMovement. Not consumed yet -- recorded because it is
    // structurally confirmed (Mesh / CharacterMovement / Capsule are adjacent in
    // UE's ACharacter) and it carries MovementMode for ballistic prediction.
    uintptr_t ACharacter_CharacterMovement = 0x0;

    // ---- collision capsule (verified live) --------------------------------
    //  The game's own per-class height; RootComponent IS the capsule on
    //  ACharacter, so extents are origin.Z +/- HalfHeight.
    uintptr_t ACharacter_CapsuleComponent = 0x0;
    uintptr_t Capsule_HalfHeight          = 0x0;   // float

    // ---- health (MEASURED, not from the SDK) ------------------------------
    //  Two adjacent floats reading 150/250/350 (Light/Medium/Heavy) across six
    //  players. Which is Current is not assumed: smaller = current, larger =
    //  max. The old SDK value 0x5C8 read a constant 100 for everyone.
    uintptr_t ADiscoveryCharacter_Health  = 0x0;   // UHealthComponent*
    uintptr_t HealthComp_MaxDouble        = 0x0;   // HealthMax, reflected
    uintptr_t HealthComp_ScanLo           = 0x0;   // trailing pad: health lives here
    uintptr_t HealthComp_ScanHi           = 0x0;
    uintptr_t Health_A                    = 0x0;   // float
    uintptr_t Health_B                    = 0x0;   // float

    // ---- squad / team (SDK values, confirmed correct in every mode) -------
    uintptr_t ADiscoveryCharacter_Squad   = 0x0;   // USquadComponent*
    uintptr_t Squad_Index                 = 0x0;   // int32
};

inline RuntimeOffsets g_off;

inline std::map<std::string, uintptr_t*> offsetFields(RuntimeOffsets& o) {
    return {
        {"GEngine_RVA",                  &o.GEngine_RVA},
        {"GObjects_RVA",                 &o.GObjects_RVA},
        {"GObjects_Key",                 &o.GObjects_Key},
        {"GObjects_NumKey",              &o.GObjects_NumKey},
        {"GObjects_ObjKey",              &o.GObjects_ObjKey},
        {"GObjects_Imm1",                &o.GObjects_Imm1},
        {"GObjects_Imm2",                &o.GObjects_Imm2},
        {"GObjects_Shift",               &o.GObjects_Shift},
        {"GObjects_NumOff",              &o.GObjects_NumOff},
        {"GObjects_ObjOff",              &o.GObjects_ObjOff},
        {"UGameEngine_GameInstance",     &o.UGameEngine_GameInstance},
        {"UGameInstance_LocalPlayers",   &o.UGameInstance_LocalPlayers},
        {"UPlayer_PlayerController",     &o.UPlayer_PlayerController},
        {"ADiscoveryCharacter_AnimSU",   &o.ADiscoveryCharacter_AnimSU},
        {"AnimSU_GameState",             &o.AnimSU_GameState},
        {"UWorld_PersistentLevel",       &o.UWorld_PersistentLevel},
        {"ULevel_OwningWorld",           &o.ULevel_OwningWorld},
        {"AController_Pawn",             &o.AController_Pawn},
        {"APawn_Controller",             &o.APawn_Controller},
        {"UWorld_NetDriver",             &o.UWorld_NetDriver},
        {"UWorld_Levels",                &o.UWorld_Levels},
        {"AController_PlayerState",      &o.AController_PlayerState},
        {"AController_ControlRotation",  &o.AController_ControlRotation},
        {"APawn_BaseEyeHeight",          &o.APawn_BaseEyeHeight},
        {"AActor_RootComponent",         &o.AActor_RootComponent},
        {"USceneComponent_RelLocation",  &o.USceneComponent_RelLocation},
        {"AGameStateBase_PlayerArray",   &o.AGameStateBase_PlayerArray},
        {"AGameStateBase_WorldTime",     &o.AGameStateBase_WorldTime},
        {"APlayerState_PawnPrivate",     &o.APlayerState_PawnPrivate},
        {"APlayerState_Spectator",       &o.APlayerState_Spectator},
        {"APlayerState_DisplayName",     &o.APlayerState_DisplayName},
        {"APlayerState_ClubTag",         &o.APlayerState_ClubTag},
        {"APlayerState_Discriminator",   &o.APlayerState_Discriminator},
        {"APlayerCameraManager_PCOwner", &o.APlayerCameraManager_PCOwner},
        {"APlayerCameraManager_POVLoc",  &o.APlayerCameraManager_POVLoc},
        {"POV_RotOffset",                &o.POV_RotOffset},
        {"POV_FovOffset",                &o.POV_FovOffset},
        {"VT_UWorld",                    &o.VT_UWorld},
        {"VT_ULevel",                    &o.VT_ULevel},
        {"VT_APlayerController",         &o.VT_APlayerController},
        {"VT_APawn",                     &o.VT_APawn},
        {"VT_APlayerState",              &o.VT_APlayerState},
        {"VT_AGameStateBase",            &o.VT_AGameStateBase},
        {"VT_APlayerCameraManager",      &o.VT_APlayerCameraManager},
        {"APawn_Mesh",                   &o.APawn_Mesh},
        {"Mesh_SkeletalMeshAsset",       &o.Mesh_SkeletalMeshAsset},
        {"SkeletalMesh_Skeleton",        &o.SkeletalMesh_Skeleton},
        {"Skeleton_ParentTable",         &o.Skeleton_ParentTable},
        {"Skeleton_BindPose",            &o.Skeleton_BindPose},
        {"Bone_Head",                    &o.Bone_Head},
        {"Mesh_BoneTree",                &o.Mesh_BoneTree},
        {"BoneTree_Parents",             &o.BoneTree_Parents},
        {"Bone_Pelvis",                  &o.Bone_Pelvis},
        {"Mesh_BoneArray",               &o.Mesh_BoneArray},
        {"Mesh_ComponentToWorld",        &o.Mesh_ComponentToWorld},
        {"Mesh_LastRenderTime",          &o.Mesh_LastRenderTime},
        {"SceneComp_ComponentVelocity",  &o.SceneComp_ComponentVelocity},
        {"ACharacter_CharacterMovement", &o.ACharacter_CharacterMovement},
        {"ACharacter_CapsuleComponent",  &o.ACharacter_CapsuleComponent},
        {"Capsule_HalfHeight",           &o.Capsule_HalfHeight},
        {"ADiscoveryCharacter_Health",   &o.ADiscoveryCharacter_Health},
        {"HealthComp_MaxDouble",         &o.HealthComp_MaxDouble},
        {"HealthComp_ScanLo",            &o.HealthComp_ScanLo},
        {"HealthComp_ScanHi",            &o.HealthComp_ScanHi},
        {"Health_A",                     &o.Health_A},
        {"Health_B",                     &o.Health_B},
        {"ADiscoveryCharacter_Squad",    &o.ADiscoveryCharacter_Squad},
        {"Squad_Index",                  &o.Squad_Index},
    };
}

// FMinimalViewInfo spacing, with the values verified on this build as the
// fallback. A config that does not mention them behaves exactly as before.
// The GObjects decode shape, with this build's verified values as the fallback
// so a config that predates these keys behaves exactly as before.
inline uint8_t   goImm1()   { return g_off.GObjects_Imm1  ? uint8_t(g_off.GObjects_Imm1)  : 0xB1; }
inline uint8_t   goImm2()   { return g_off.GObjects_Imm2  ? uint8_t(g_off.GObjects_Imm2)  : 0x39; }
inline int       goShift()  { return g_off.GObjects_Shift ? int(g_off.GObjects_Shift)     : 5; }
inline uintptr_t goNumOff() { return g_off.GObjects_NumOff ? g_off.GObjects_NumOff        : 0x0C; }
inline uintptr_t goObjOff() { return g_off.GObjects_ObjOff ? g_off.GObjects_ObjOff        : 0x20; }

inline uintptr_t povRotOff() {
    return g_off.POV_RotOffset ? g_off.POV_RotOffset : 0x20;
}
inline uintptr_t povFovOff() {
    return g_off.POV_FovOffset ? g_off.POV_FovOffset : 0x48;
}

inline const char* kOffsetsFile = "offsets.cfg";

// Searched in the working directory and one level up, so the same binary works
// whether it is launched from the project root or from build/.
inline const char* findOffsetsFile() {
    static const char* candidates[] = { "offsets.cfg", "../offsets.cfg" };
    for (const char* c : candidates) {
        std::ifstream probe(c);
        if (probe) return c;
    }
    return nullptr;
}

inline bool loadOffsets(const char* path = nullptr) {
    if (!path) path = findOffsetsFile();
    if (!path) return false;
    std::ifstream f(path);
    if (!f) return false;
    auto fields = offsetFields(g_off);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!k.empty() && isspace((unsigned char)k.back()))  k.pop_back();
        while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
        auto it = fields.find(k);
        if (it == fields.end()) continue;
        *it->second = (uintptr_t)strtoull(v.c_str(), nullptr, 0);
        n++;
    }
    printf("[offsets] loaded %d value(s) from %s\n", n, path);
    return n > 0;
}

// Offsets are the whole tool: with them zero every read lands on nothing and the
// ESP draws an empty screen with no explanation. Name the missing ones instead.
inline bool offsetsSane() {
    struct { const char* name; uintptr_t v; } required[] = {
        { "GObjects_RVA",                g_off.GObjects_RVA },
        { "AController_Pawn",            g_off.AController_Pawn },
        { "APawn_Controller",            g_off.APawn_Controller },
        { "AActor_RootComponent",        g_off.AActor_RootComponent },
        { "USceneComponent_RelLocation", g_off.USceneComponent_RelLocation },
        { "AGameStateBase_PlayerArray",  g_off.AGameStateBase_PlayerArray },
        { "APlayerState_PawnPrivate",    g_off.APlayerState_PawnPrivate },
        { "APawn_Mesh",                  g_off.APawn_Mesh },
    };
    int missing = 0;
    for (auto& r : required)
        if (!r.v) { printf("[offsets] MISSING: %s\n", r.name); missing++; }
    return missing == 0;
}
