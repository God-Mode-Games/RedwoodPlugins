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
   * its ground-trace hit. When the world's game mode exposes an ACharacter
   * default pawn, this is the capsule half-height plus SpawnClearanceMargin,
   * and never less than the upstream 100 units. Without one, the upstream
   * 100 units. The tolerance to a trace hit below the true floor (a bevel
   * seam, a grating) is lift minus half-height: upstream gave the game's
   * measured 88-unit capsule only 12 units; this gives it
   * SpawnClearanceMargin, and scales for a taller pawn, where upstream
   * embedded it. Public so the automation test pins both branches.
   */
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  float GetSpawnGroundClearance() const;

  // FORK(hollowed-oath): the below-grade tolerance for the ground trace. A
  // spawn set this far above the capsule's rest point drops in harmlessly;
  // a hit deeper below grade than this embeds the capsule, and the game
  // mode's pawn-spawn recovery takes over.
  static constexpr float SpawnClearanceMargin = 24.0f;

  // FORK(hollowed-oath): the upstream lift; the floor of every clearance.
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
