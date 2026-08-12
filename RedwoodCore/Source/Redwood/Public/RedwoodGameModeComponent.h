// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "SocketIONative.h"

#include "RedwoodGameModeComponent.generated.h"

class URedwoodServerGameSubsystem;

// FORK(hollowed-oath): fork-added delegate, not in upstream Redwood. Part of
// the linkdead pawn retention feature (HollowedOath#1365 binds it from
// AHollowedOathGameMode; pairs with the retainBinding field RedwoodBackend#18
// adds to the player-left schema). Keep across upstream merges.
//
// Return true when this exiting player's character remains in-world after
// the connection (e.g. a retained linkdead body): the player-left is still
// emitted immediately — presence (party/chat/director) clears exactly as
// stock — but it carries retainBinding, so the backend keeps the
// character->instance write binding alive for the game's final flush. The
// game then calls URedwoodServerGameSubsystem::EmitPlayerLeft (no flag) when
// the character actually leaves the world, releasing the binding.
DECLARE_DELEGATE_RetVal_OneParam(
  bool, FRedwoodShouldRetainCharacterBinding, APlayerController *
);

UCLASS()
class REDWOOD_API URedwoodGameModeComponent : public UActorComponent {
  GENERATED_BODY()

public:
  URedwoodGameModeComponent(const FObjectInitializer &ObjectInitializer);

  //~UActorComponent interface
  virtual void BeginPlay() override;
  virtual void TickComponent(
    float DeltaTime,
    enum ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction
  ) override;
  //~End of UActorComponent interface

  //~AGameModeBase interface
  void InitGame(
    const FString &MapName, const FString &Options, FString &ErrorMessage
  );

  APlayerController *Login(
    UPlayer *NewPlayer,
    ENetRole InRemoteRole,
    const FString &Portal,
    const FString &Options,
    const FUniqueNetIdRepl &UniqueId,
    FString &ErrorMessage,
    std::function<APlayerController
                    *(UPlayer *,
                      ENetRole,
                      const FString &,
                      const FString &,
                      const FUniqueNetIdRepl &,
                      FString &)> SuperDelegate
  );

  void PostLogin(APlayerController *NewPlayer);

  bool PlayerCanRestart_Implementation(
    APlayerController *Player,
    std::function<bool(APlayerController *)> SuperDelegate
  );

  void FinishRestartPlayer(
    AController *NewPlayer,
    const FRotator &StartRotation,
    std::function<void(AController *)> FailedToRestartPlayerDelegate
  );

  FTransform PickPawnSpawnTransform(
    AController *NewPlayer, const FTransform &SpawnTransform
  );

  /**
   * FORK(hollowed-oath): recovery for a failed default-pawn spawn. Upstream
   * passes a null return from SpawnDefaultPawnAtTransform through unchanged,
   * which leaves the arriving player a pawnless spectator with no retry and
   * no feedback (observed live: a zone-spawn trace hit on a bevel seam put
   * the capsule into the floor and the engine refused the spawn). Two
   * attempts, in order:
   *   1. The same transform lifted by the pawn capsule's half-height.
   *   2. The map's PlayerStart (FindPlayerStart), force-spawned with
   *      AdjustIfPossibleButAlwaysSpawn so the player always gets a pawn.
   * Both game mode variants call this when Super returns null. An upstream
   * merge must keep those call sites.
   */
  APawn *RetryFailedPawnSpawn(
    AGameModeBase *GameMode,
    AController *NewPlayer,
    const FTransform &FailedTransform
  );
  //~End of AGameModeBase interface

  UFUNCTION()
  void PostBeginPlay();

  UFUNCTION(BlueprintCallable, Category = "Redwood|GameMode")
  TArray<FString> GetExpectedCharacterIds() const;

  UFUNCTION(BlueprintCallable, Category = "Redwood|GameMode")
  void OnGameModeLogout(AGameModeBase *GameMode, AController *Controller);

  // FORK(hollowed-oath): fork-added member (see the delegate declaration
  // above). Optional gate a game can bind for players whose in-world
  // presence outlives the connection (e.g. linkdead body retention).
  // Unbound = stock behavior.
  FRedwoodShouldRetainCharacterBinding ShouldRetainCharacterBinding;

  UFUNCTION()
  void FlushPersistence();

  void InitVariables(
    float InDatabasePersistenceInterval, float InPostBeginPlayDelay
  ) {
    DatabasePersistenceInterval = InDatabasePersistenceInterval;
    PostBeginPlayDelay = InPostBeginPlayDelay;
  };

private:
  /**
   * Authoritative core of player-auth verification: hits the sidecar
   * with {playerId, characterId, token} and either marks the
   * PlayerController as ready (PlayerStateComponent->SetServerReady)
   * or kicks. Called from `Login()` once the join token has been
   * parsed from the connection URL options.
   */
  void RunSidecarPlayerAuth(
    APlayerController *PlayerController,
    const FString &PlayerId,
    const FString &CharacterId,
    const FString &Token
  );

  float DatabasePersistenceInterval;
  float PostBeginPlayDelay;

  TSharedPtr<FSocketIONative> Sidecar;

  FTimerHandle FlushPersistentDataTimerHandle;
  FTimerHandle PostBeginPlayTimerHandle;

  bool bPostBeganPlay = false;

  UPROPERTY()
  URedwoodServerGameSubsystem *ServerSubsystem = nullptr;
};
