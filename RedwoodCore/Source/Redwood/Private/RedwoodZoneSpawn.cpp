// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodZoneSpawn.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
// FORK(hollowed-oath): for the capsule-aware ground clearance in
// GetSpawnTransform.
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"

// FORK(hollowed-oath): see the rationale on the header declaration.
float ARedwoodZoneSpawn::GetSpawnGroundClearance() const {
  if (const AGameModeBase *GameMode = GetWorld()->GetAuthGameMode()) {
    const ACharacter *PawnDefault = GameMode->DefaultPawnClass
      ? Cast<ACharacter>(GameMode->DefaultPawnClass->GetDefaultObject())
      : nullptr;
    if (PawnDefault && PawnDefault->GetCapsuleComponent()) {
      return PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
        SpawnClearanceMargin;
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

  // FORK(hollowed-oath): the upstream fixed 100-unit lift gives a
  // taller-than-88 capsule almost no clearance, and a trace hit on a bevel
  // seam or grating puts the capsule into the floor — the engine then
  // refuses the pawn spawn and the player arrives pawnless. Lift by the
  // default pawn's capsule half-height plus a margin when the game mode
  // exposes a Character pawn; keep the upstream lift otherwise. An upstream
  // merge must keep GetSpawnGroundClearance (the automation test pins it).
  Location += FVector(0.0f, 0.0f, GetSpawnGroundClearance());

  Transform.SetLocation(Location);

  if (bRandomizeRotation) {
    Transform.SetRotation(FQuat(FRotator(0.0f, FMath::FRand() * 360.0f, 0.0f)));
  }

  return Transform;
}