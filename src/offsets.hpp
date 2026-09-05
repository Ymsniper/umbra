#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Engine layout constants that do not change between game builds
// (TArray and FString shape, bone indices).
#include <cstdint>

// ONE SOURCE OF TRUTH: offsets.cfg

namespace offsets {
    // ── UWorld static pointer (offset from module base)
    constexpr uintptr_t GWorldOffset = 0xda2e418; // GWorld - found via IDA pattern scan

    // Given by the person who provided the SDK dump. Almost certainly
    constexpr uint64_t RVA_FN_GET_WORLD_FROM_CTX = 0x498F240ULL;

    // ── UWorld
    namespace UWorld {
        // VERIFIED live (pid 5085, in-match, 2026-08-24): the cycle
        // [World+0x158] -> Level and [Level+0x138] -> World closes exactly.
        constexpr uintptr_t PersistentLevel    = 0x0158; // ULevel*          (verified live)
        constexpr uintptr_t NetDriver          = 0x0140; // UNetDriver*      (verified live, non-null in match)
        constexpr uintptr_t Levels             = 0x0408; // TArray<ULevel*>  (verified live, Num=74 in match)
        constexpr uintptr_t LevelCollections   = 0x02A8; // TArray<FLevelCollection>

        constexpr uintptr_t OwningGameInstance = 0x04B0; // UGameInstance*   (measured, not confirmed)
        constexpr uintptr_t OwningGameInstanceCandidates[] = { 0x04B0, 0x04A8, 0x04C0 };

        constexpr uintptr_t AuthorityGameMode  = 0x0470; // AGameModeBase*   (null on clients - cannot verify)
    }

    // ── ULevel
    namespace ULevel {
        constexpr uintptr_t OwningWorld = 0x0138; // UWorld* back-reference (SDK confirmed)
    }

    // ── UGameInstance
    namespace UGameInstance {
        constexpr uintptr_t LocalPlayers = 0x00C0; // TArray<ULocalPlayer*>
    }

    // ── GEngine chain (preferred: avoids encrypted GWorld)
    namespace UEngine {
        constexpr uintptr_t GameInstance  = 0x1530; // UEmbarkGameEngine::GameInstance (ID marker)
        // GameViewport offset is build-specific; resolved at runtime by --find-engine.
        // Common UE5 range is 0x0840..0x0880. Set once found:
        constexpr uintptr_t GameViewport  = 0x0000; // TODO: fill from --find-engine output
    }
    namespace UGameViewportClient {
        constexpr uintptr_t GameInstance = 0x00F8; // SDK confirmed
        constexpr uintptr_t World        = 0x0168; // SDK confirmed
    }

    // ── GObjects / GUObjectArray (UE5 FChunkedFixedUObjectArray)
    namespace GObjects {
        constexpr uintptr_t FUObjectItem_Size    = 0x18;   // 24 bytes per item (UE5)
        constexpr uintptr_t ChunkSize            = 65536;  // 0x10000 objects per chunk
        // Field offsets within FChunkedFixedUObjectArray:
        constexpr uintptr_t ObjectsPtr           = 0x00;   // FUObjectItem** (chunk list)
        constexpr uintptr_t MaxElements          = 0x10;   // int32
        constexpr uintptr_t NumElements          = 0x14;   // int32
        constexpr uintptr_t MaxChunks            = 0x18;   // int32
        constexpr uintptr_t NumChunks            = 0x1C;   // int32
    }
    // ── UObject header - DO NOT NAVIGATE THROUGH THESE ON THIS BUILD
    namespace UObjectHdr {
        constexpr uintptr_t VTable        = 0x00;   // valid
        constexpr uintptr_t ObjectFlags   = 0x08;   // unverified
        constexpr uintptr_t InternalIndex = 0x0C;   // unverified
        constexpr uintptr_t ClassPrivate  = 0x10;   // READS ZERO - do not use
        constexpr uintptr_t NamePrivate   = 0x18;   // READS ZERO - do not use
        constexpr uintptr_t OuterPrivate  = 0x20;   // not a pointer - do not use
    }

    // ── vtable RVAs - verified live, module base 0x140000000 == .i64 base ───
    //  Identify objects by vtable identity since the header is unusable.
    //  find_uworld's Strategy D re-derives UWorld/ULevel every run, so these
    //  are for reference and for fast paths.
    namespace VTableRVA {
        constexpr uintptr_t UWorld     = 0xD6D4E50;  // IDA 0x14D6D4E50, 24/24 slots -> .text
        constexpr uintptr_t ULevel     = 0xD305130;  // IDA 0x14D305130, 24/24
        constexpr uintptr_t UNetDriver = 0xDBA5DD0;  // IDA 0x14DBA5DD0
    }

    // ── UPlayer (base of ULocalPlayer)
    namespace UPlayer {
        constexpr uintptr_t PlayerController = 0x00A0; // APlayerController*
    }

    // ── AController (base of APlayerController)
    namespace AController {
        constexpr uintptr_t PlayerState     = 0x03F8; // APlayerState*   (verified: real vtable)
        constexpr uintptr_t Character       = 0x0428; // ACharacter*     (verified: == Pawn)
        constexpr uintptr_t Pawn            = 0x0430; // APawn*          (verified: cycle closes)
        constexpr uintptr_t ControlRotation = 0x0440; // FRotator, 3 DOUBLES (verified: roll==0.0,
                                                     // values track mouse movement live)
    }
    // ── skeleton (VERIFIED live 2026-08-25)
    namespace USkeletalMeshComponent {
        constexpr uintptr_t BoneArray       = 0x0B08; // TArray<FTransform>, BONE-space
        constexpr uintptr_t ComponentToWorld = 0x0320; // FTransform
    }
    namespace FTransformLayout {
        constexpr uintptr_t Size        = 96;
        constexpr uintptr_t Rotation    = 0x00;   // FQuat, 4 doubles
        constexpr uintptr_t Translation = 0x20;   // FVector, 3 doubles
        constexpr uintptr_t Scale3D     = 0x40;   // FVector, 3 doubles
    }

    namespace APawn {
        constexpr uintptr_t Controller    = 0x0408;   // AController*    (verified: cycle closes)
        constexpr uintptr_t PlayerState   = 0x03F8;   // APlayerState*
        constexpr uintptr_t BaseEyeHeight = 0x03E4;   // float
        constexpr uintptr_t Mesh          = 0x0458;
    }

    // ── APlayerController
    namespace APlayerController {
        constexpr uintptr_t PlayerCameraManager = 0x0488; // APlayerCameraManager* (VERIFY)
    }

    // ── APlayerCameraManager
    namespace APlayerCameraManager {
        constexpr uintptr_t PCOwner    = 0x0458; // APlayerController* back-ref (verified)
        constexpr uintptr_t ViewTarget = 0x04A0; // FTViewTarget (verified via POV)
    }
    // FTViewTarget layout: {AActor* 0x0, APlayerController* 0x8, FMinimalViewInfo POV @ 0x10}
    namespace FTViewTarget {
        constexpr uintptr_t POV = 0x0010; // FMinimalViewInfo
    }
    // FMinimalViewInfo layout (from Struct.cpp):
    namespace FMinimalViewInfo {
        constexpr uintptr_t Location = 0x0008; // FVector (3 doubles)
        constexpr uintptr_t Rotation = 0x0030; // FRotator (3 doubles: pitch/yaw/roll)
        constexpr uintptr_t FOV      = 0x0058; // float
    }

    // ── AGameModeBase
    namespace AGameModeBase {
        constexpr uintptr_t GameState = 0x0438; // AGameStateBase*
    }

    // ── AGameStateBase
    namespace AGameStateBase {
        constexpr uintptr_t PlayerArray = 0x04E0; // TArray<APlayerState*>
    }

    // ── APlayerState
    namespace APlayerState {
        constexpr uintptr_t Score             = 0x03D8; // float
        constexpr uintptr_t bIsSpectator_byte = 0x03E2; // bitfield byte (bit 1 = bIsSpectator)
        constexpr uintptr_t PawnPrivate       = 0x0450; // APawn*  (→ ADiscoveryCharacter)
        constexpr uintptr_t PlayerNamePrivate = 0x0470; // FString - GARBAGE on this game, unused
        constexpr uintptr_t Discriminator     = 0x05D8; // FString, the "#1234" suffix   (verified)
        constexpr uintptr_t EmbarkPlayerId    = 0x0668; // FString UUID                  (verified)
        constexpr uintptr_t ClubTag           = 0x0688; // FString, may be empty         (verified)
        constexpr uintptr_t DisplayName       = 0x0698; // FString - USE THIS for ESP    (verified)
        constexpr uintptr_t SessionId         = 0x0740; // FString UUID                  (verified)
    }

    // ── AActor
    namespace AActor {
        constexpr uintptr_t RootComponent = 0x0258; // USceneComponent*  (verified live)
    }

    // ── vtable RVAs for the player chain - verified live
    namespace PlayerVTableRVA {
        constexpr uintptr_t APlayerController = 0xDF613B0; // IDA 0x14DF613B0 (unique on client)
        constexpr uintptr_t APawn             = 0xDF45540; // IDA 0x14DF45540
        constexpr uintptr_t APlayerState      = 0xDE56CA0; // IDA 0x14DE56CA0
        constexpr uintptr_t USceneComponent   = 0xD44B450; // IDA 0x14D44B450 (root component)
        constexpr uintptr_t AGameStateBase    = 0xDE49CC0; // IDA 0x14DE49CC0 (verified: its
                                                          // PlayerArray@+0x4E0 held 16 PlayerStates)
        constexpr uintptr_t APlayerCameraManager = 0xE035A40; // IDA 0x14E035A40 (verified:
                                                          // POV 128 units from player, FOV 100)
    }

    // ── USceneComponent
    namespace USceneComponent {
        constexpr uintptr_t RelativeLocation = 0x0278; // FVector (3 × double)
        constexpr uintptr_t RelativeRotation = 0x0290; // FRotator (3 × double)
    }

    // ── ACharacter (inherits APawn → AActor)
    namespace UCapsuleComponent {
        constexpr uintptr_t CapsuleHalfHeight = 0x06B0; // float
        constexpr uintptr_t CapsuleRadius     = 0x06B4; // float
    }

    namespace ACharacter {
        constexpr uintptr_t CapsuleComponent = 0x0468; // UCapsuleComponent*
        constexpr uintptr_t Mesh = 0x0458; // USkeletalMeshComponent*
    }

    // ── USkinnedMeshComponent / UEmbarkSkeletalMeshComponent
    namespace USkeletalMesh {
        constexpr uintptr_t CachedComponentSpaceTransforms = 0x0B98; // TArray<FTransform>
        // ComponentToWorld (FTransform) - stored internally, not as UPROPERTY.
        // Read via: USceneComponent::RelativeLocation on the mesh component
        // OR use the 0x01C0 slot (engine internal, verify with CE):
        constexpr uintptr_t ComponentToWorld = 0x01C0; // FTransform (48 bytes) - engine internal
    }

    // Bone indices for ADiscoveryCharacter (verify with bone-name dump if they shift)
    namespace Bones {
        constexpr int Head      = 8;
        constexpr int Neck      = 7;
        constexpr int Chest     = 5;
        constexpr int Pelvis    = 2;
        constexpr int LShoulder = 34;
        constexpr int RShoulder = 9;
        constexpr int LHand     = 39;
        constexpr int RHand     = 14;
        constexpr int LKnee     = 75;
        constexpr int RKnee     = 68;
        constexpr int LFoot     = 77;
        constexpr int RFoot     = 70;
    }

    // ── ADiscoveryCharacter
    namespace ADiscoveryCharacter {
        constexpr uintptr_t HealthComponent = 0x0D10; // UDiscoveryCharacterHealthComponent*
        constexpr uintptr_t SquadComponent  = 0x0DB0; // USquadComponent*
    }

    // ── UHealthComponent (base)
    namespace UHealthComponent {
        constexpr uintptr_t HealthA = 0x0730; // float
        constexpr uintptr_t HealthB = 0x0734; // float
    }

    // ── USquadComponent
    namespace USquadComponent {
        constexpr uintptr_t SquadIndex        = 0x0170; // int32
        constexpr uintptr_t PlayerIndexInSquad= 0x0174; // int32
    }

    // ── TArray helpers
    constexpr uintptr_t TArray_Data = 0x00;
    constexpr uintptr_t TArray_Num  = 0x08;

    // ── FString helpers
    constexpr uintptr_t FString_Data = 0x00;
    constexpr uintptr_t FString_Len  = 0x08;

} // namespace offsets
