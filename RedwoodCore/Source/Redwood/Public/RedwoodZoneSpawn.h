// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "RedwoodZoneSpawn.generated.h"

UCLASS(BlueprintType, Blueprintable)
class REDWOOD_API ARedwoodZoneSpawn : public AActor {
  GENERATED_BODY()

public:
  ARedwoodZoneSpawn(
    const FObjectInitializer &ObjectInitializer = FObjectInitializer::Get()
  );

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  FTransform GetSpawnTransform();

  /**
   * FORK(hollowed-oath): the vertical lift GetSpawnTransform applies above
   * its ground-trace hit. The default pawn's capsule half-height plus
   * SpawnClearanceMargin when the world's game mode exposes an ACharacter
   * default pawn; the upstream fixed 100 units otherwise. Public so the
   * automation test pins both branches.
   */
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  float GetSpawnGroundClearance() const;

  // FORK(hollowed-oath): margin above the capsule half-height, so a hit on
  // a slightly below-grade surface (bevel seam, grating) still leaves the
  // engine's spawn-adjust room to place the capsule.
  static constexpr float SpawnClearanceMargin = 4.0f;

  // FORK(hollowed-oath): the upstream lift, kept for non-Character pawns.
  static constexpr float LegacySpawnGroundClearance = 100.0f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Redwood")
  FString ZoneName;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Redwood")
  FString SpawnName = TEXT("default");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Redwood")
  float SpawnRadius = 0.0f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Redwood")
  bool bRandomizeRotation = false;
};
