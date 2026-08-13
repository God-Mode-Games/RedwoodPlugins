// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodZoneSpawn.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
// FORK(hollowed-oath): for the capsule-aware ground clearance in
// GetSpawnTransform.
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"

// FORK(hollowed-oath): see the rationale on the header declaration. No
// controller exists at this call site, so the game mode's static
// DefaultPawnClass stands in for GetDefaultPawnClassForController.
float ARedwoodZoneSpawn::GetSpawnGroundClearance() const {
  const UWorld *World = GetWorld();
  const AGameModeBase *GameMode = World ? World->GetAuthGameMode() : nullptr;
  if (GameMode) {
    const ACharacter *PawnDefault = GameMode->DefaultPawnClass
      ? Cast<ACharacter>(GameMode->DefaultPawnClass->GetDefaultObject())
      : nullptr;
    if (PawnDefault && PawnDefault->GetCapsuleComponent()) {
      return FMath::Max(
        PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
          SpawnClearanceMargin,
        LegacySpawnGroundClearance
      );
    }
  }
  return LegacySpawnGroundClearance;
}

ARedwoodZoneSpawn::ARedwoodZoneSpawn(const FObjectInitializer &ObjectInitializer
) :
  Super(ObjectInitializer) {
  RootComponent =
    CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
  SetRootComponent(RootComponent);

  // add a billboard component
  UBillboardComponent *BillboardComponent =
    CreateDefaultSubobject<UBillboardComponent>(TEXT("BillboardComponent"));
  BillboardComponent->SetupAttachment(RootComponent);

  // add a arrow component
  UArrowComponent *ArrowComponent =
    CreateDefaultSubobject<UArrowComponent>(TEXT("ArrowComponent"));
  ArrowComponent->SetupAttachment(RootComponent);
  ArrowComponent->ArrowColor = FColor::Red;
  ArrowComponent->ArrowSize = 1.0f;
  ArrowComponent->ArrowLength = 100.0f;
  ArrowComponent->bTreatAsASprite = true;
  ArrowComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 50.0f));
}

FTransform ARedwoodZoneSpawn::GetSpawnTransform() {
  FTransform Transform = GetActorTransform();
  FVector Location = Transform.GetLocation();

  if (SpawnRadius > 0.0f) {
    float RandomAngle = FMath::FRand() * 360.0f;
    float RandomRadius = FMath::FRand() * SpawnRadius;

    Location += FVector(
      FMath::Cos(RandomAngle) * RandomRadius,
      FMath::Sin(RandomAngle) * RandomRadius,
      0.0f
    );
  }

  // Make sure the location is at ground level
  FHitResult HitResult;
  if (GetWorld()->LineTraceSingleByChannel(
        HitResult,
        Location,
        Location - FVector(0.0f, 0.0f, 1000.0f),
        ECC_WorldStatic
      )) {
    Location = HitResult.Location;
  } else if (GetWorld()->LineTraceSingleByChannel(
               HitResult,
               Location + FVector(0.0f, 0.0f, 1000.0f),
               Location,
               ECC_WorldStatic
             )) {
    Location = HitResult.Location;
  }

  // FORK(hollowed-oath): the upstream fixed 100-unit lift left the game's
  // measured 88-unit capsule only 12 units of below-grade tolerance — a
  // trace hit on a bevel seam or a grating put the capsule into the floor,
  // the engine refused the pawn spawn, and the player arrived pawnless
  // (observed live). GetSpawnGroundClearance widens that tolerance and
  // scales it for taller pawns; it never goes below the upstream lift. An
  // upstream merge must keep it (the automation test pins both branches).
  Location += FVector(0.0f, 0.0f, GetSpawnGroundClearance());

  Transform.SetLocation(Location);

  if (bRandomizeRotation) {
    Transform.SetRotation(FQuat(FRotator(0.0f, FMath::FRand() * 360.0f, 0.0f)));
  }

  return Transform;
}