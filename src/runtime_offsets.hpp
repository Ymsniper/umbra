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
#include <initializer_list>
#include <utility>

// Runtime offsets
struct RuntimeOffsets {
    // ---- verified by a closed two-hop cycle -------------------------------
 // ---- the GEngine chain: every live object by pointer, no heap sweep -------
    uintptr_t GEngine_RVA                 = 0x0;   // static; NEVER FOUND on this build
    uintptr_t GObjects_RVA                = 0x0;   // the 128-bit module global
    uintptr_t GObjects_KeyA               = 0x0;   // the caller's first key
    uintptr_t GObjects_KeyB               = 0x0;   // the caller's second key
    uintptr_t GObjects_NumKey             = 0x0;   // NumElements XOR, bswapped
    uintptr_t GObjects_ObjKey             = 0x0;   // Objects XOR, bswapped
    uintptr_t GObjects_NumOff             = 0x0;   // NumElements member offset
    uintptr_t GObjects_ObjOff             = 0x0;   // Objects member offset
    uintptr_t GObjects_EntryBase          = 0x0;   // object pointer inside an entry
    uintptr_t GObjects_Stride             = 0x0;   // bytes per entry
    uintptr_t GObjects_IndexOff           = 0x0;   // where an object holds its index
    uintptr_t GObjects_ChunkShift         = 0x0;   // index >> this = the chunk
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
    // APlayerController::PlayerCameraManager. Clients spawn a camera manager
    // too, so the controller's own pointer reaches it without a heap sweep.
    uintptr_t APlayerController_CameraManager = 0x0;
    // AEmbarkPlayerStateBase::Squad, the AEmbarkSquad actor squadmates share.
    uintptr_t APlayerState_Squad          = 0x0;

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

    // ---- health -----------------------------------------------------------
    //  Two adjacent floats, holding the class totals 150, 250 and 350 for
    //  Light, Medium and Heavy. Which one is current is not assumed: the
    //  smaller is current and the larger is max, on every player and frame.
    uintptr_t ADiscoveryCharacter_Health  = 0x0;   // UHealthComponent*
    uintptr_t HealthComp_MaxDouble        = 0x0;   // HealthMax, reflected
    uintptr_t HealthComp_ScanLo           = 0x0;   // trailing pad: health lives here
    uintptr_t HealthComp_ScanHi           = 0x0;
    uintptr_t Health_A                    = 0x0;   // float
    uintptr_t Health_B                    = 0x0;   // float

    // ---- squad / team, through the component on the pawn ------------------
    uintptr_t ADiscoveryCharacter_Squad   = 0x0;   // USquadComponent*
    uintptr_t Squad_Index                 = 0x0;   // int32
};

inline RuntimeOffsets g_off;

inline std::map<std::string, uintptr_t*> offsetFields(RuntimeOffsets& o) {
    return {
        {"GEngine_RVA",                  &o.GEngine_RVA},
        {"GObjects_RVA",                 &o.GObjects_RVA},
        {"GObjects_KeyA",                &o.GObjects_KeyA},
        {"GObjects_KeyB",                &o.GObjects_KeyB},
        {"GObjects_NumKey",              &o.GObjects_NumKey},
        {"GObjects_ObjKey",              &o.GObjects_ObjKey},
        {"GObjects_NumOff",              &o.GObjects_NumOff},
        {"GObjects_ObjOff",              &o.GObjects_ObjOff},
        {"GObjects_EntryBase",           &o.GObjects_EntryBase},
        {"GObjects_Stride",              &o.GObjects_Stride},
        {"GObjects_IndexOff",            &o.GObjects_IndexOff},
        {"GObjects_ChunkShift",          &o.GObjects_ChunkShift},
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
        {"APlayerController_CameraManager", &o.APlayerController_CameraManager},
        {"APlayerState_Squad",           &o.APlayerState_Squad},
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
    std::map<std::string, uintptr_t> seen;
    std::string line;
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
        const uintptr_t val = (uintptr_t)strtoull(v.c_str(), nullptr, 0);
        // A key written twice is a hand edit on top of a derived file, and
        // only the later line counts. Say so, or the value that looks right in
        // the file is not the one being used.
        auto dup = seen.find(k);
        if (dup != seen.end() && dup->second != val)
            printf("[offsets] %s is set twice (0x%lX, then 0x%lX); the later "
                   "line wins\n", k.c_str(), (unsigned long)dup->second,
                   (unsigned long)val);
        seen[k] = val;
        *it->second = val;
    }
    printf("[offsets] loaded %zu value(s) from %s\n", seen.size(), path);
    return !seen.empty();
}

// Offsets are the whole tool, but a box needs only a few of them: the
// controller and pawn that point at each other, the player list and each
// player's pawn, where a pawn stands, and a camera. Without one of those
// nothing can be found or projected, so startup stops and names it.
//
// Every other offset serves one feature, and a zero there is not harmless by
// itself: the read still happens, lands on the object's vtable pointer, and
// uses that as the member. For the squad that pointer is the same on every
// pawn, so every player becomes a squadmate and nothing is drawn. The reads
// check their own offset first, and this lists once what is switched off.
inline bool offsetsSane() {
    struct { const char* name; uintptr_t v; } required[] = {
        { "AController_Pawn",            g_off.AController_Pawn },
        { "APawn_Controller",            g_off.APawn_Controller },
        { "AActor_RootComponent",        g_off.AActor_RootComponent },
        { "USceneComponent_RelLocation", g_off.USceneComponent_RelLocation },
        { "AGameStateBase_PlayerArray",  g_off.AGameStateBase_PlayerArray },
        { "APlayerState_PawnPrivate",    g_off.APlayerState_PawnPrivate },
    };
    int missing = 0;
    for (auto& r : required)
        if (!r.v) { printf("[offsets] MISSING: %s\n", r.name); missing++; }
    const bool pov = g_off.APlayerCameraManager_PCOwner && g_off.APlayerCameraManager_POVLoc;
    if (!pov && !g_off.AController_ControlRotation) {
        printf("[offsets] MISSING: a camera. Either APlayerCameraManager_PCOwner "
               "and APlayerCameraManager_POVLoc, or AController_ControlRotation\n");
        missing++;
    }
    if (missing) return false;

    auto feature = [](const char* what,
                      std::initializer_list<std::pair<const char*, uintptr_t>> need) {
        std::string none;
        for (auto& n : need)
            if (!n.second) { if (!none.empty()) none += ", "; none += n.first; }
        if (!none.empty())
            printf("[offsets] off: %s (no %s)\n", what, none.c_str());
    };
    feature("the game's spectator flag, so only a missing health marks a spectator",
            {{"APlayerState_Spectator", g_off.APlayerState_Spectator}});
    if (!g_off.APlayerState_Squad &&
        !(g_off.ADiscoveryCharacter_Squad && g_off.Squad_Index))
        printf("[offsets] off: squads, so squadmates are drawn and aimed at like "
               "anyone else (no APlayerState_Squad, and no ADiscoveryCharacter_Squad "
               "with Squad_Index)\n");
    feature("health bars, skipping the dead and spotting spectators by missing health",
            {{"ADiscoveryCharacter_Health", g_off.ADiscoveryCharacter_Health},
             {"Health_A", g_off.Health_A}, {"Health_B", g_off.Health_B}});
    feature("the visibility check, so everyone counts as visible",
            {{"Mesh_LastRenderTime", g_off.Mesh_LastRenderTime}});
    feature("names", {{"APlayerState_DisplayName", g_off.APlayerState_DisplayName}});
    feature("the head from the bones, so boxes come from the capsule",
            {{"APawn_Mesh", g_off.APawn_Mesh},
             {"Mesh_BoneArray", g_off.Mesh_BoneArray},
             {"Mesh_ComponentToWorld", g_off.Mesh_ComponentToWorld}});
    if (!(g_off.Mesh_BoneTree && g_off.BoneTree_Parents))
        feature("the skeleton",
                {{"Mesh_SkeletalMeshAsset", g_off.Mesh_SkeletalMeshAsset},
                 {"SkeletalMesh_Skeleton", g_off.SkeletalMesh_Skeleton}});
    feature("the capsule height",
            {{"ACharacter_CapsuleComponent", g_off.ACharacter_CapsuleComponent},
             {"Capsule_HalfHeight", g_off.Capsule_HalfHeight}});
    feature("eye height", {{"APawn_BaseEyeHeight", g_off.APawn_BaseEyeHeight}});
    feature("reading velocity, so it is measured from movement",
            {{"SceneComp_ComponentVelocity", g_off.SceneComp_ComponentVelocity}});
    if (g_off.AController_ControlRotation)
        feature("the game's camera, so the view is built from ControlRotation and "
                "the FOV setting",
                {{"APlayerCameraManager_PCOwner", g_off.APlayerCameraManager_PCOwner},
                 {"APlayerCameraManager_POVLoc", g_off.APlayerCameraManager_POVLoc}});
    feature("reaching the camera from the controller, so it is matched by its "
            "back-pointer among the objects instead",
            {{"APlayerController_CameraManager", g_off.APlayerController_CameraManager}});
    feature("reaching the GameState from our pawn, so a heap sweep finds it",
            {{"ADiscoveryCharacter_AnimSU", g_off.ADiscoveryCharacter_AnimSU},
             {"AnimSU_GameState", g_off.AnimSU_GameState}});
    feature("picking the GameState by its match clock",
            {{"AGameStateBase_WorldTime", g_off.AGameStateBase_WorldTime}});
    feature("checking the local controller's PlayerState",
            {{"AController_PlayerState", g_off.AController_PlayerState}});
    feature("the object array, so the heap is swept to find the player",
            {{"GObjects_RVA", g_off.GObjects_RVA},
             {"GObjects_KeyA", g_off.GObjects_KeyA},
             {"GObjects_KeyB", g_off.GObjects_KeyB},
             {"GObjects_ChunkShift", g_off.GObjects_ChunkShift}});
    return true;
}
