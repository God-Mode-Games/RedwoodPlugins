// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added — no upstream counterpart.
// Pins the three zone-travel hardening contracts (HollowedOath#1752 items
// 4, 7, 8):
//   1. URedwoodPlayerStateComponent::AbortTransferring rolls back the
//      bTransferring latch that InitTransferring sets before the
//      TravelPlayerToZone* sidecar checks.
//   2. ARedwoodZoneSpawn::GetSpawnGroundClearance lifts by the default
//      pawn's capsule half-height (plus margin) when the game mode exposes
//      an ACharacter pawn, and keeps the upstream 100 units otherwise.
//   3. URedwoodGameModeComponent::RetryFailedPawnSpawn recovers a failed
//      default-pawn spawn instead of leaving the player a pawnless
//      spectator.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodZoneSpawn.h"

#if WITH_AUTOMATION_WORKER

namespace RedwoodZoneTravelTest {

struct FScopedWorld {
  UGameInstance *GameInstance = nullptr;
  UWorld *World = nullptr;

  // A standalone game instance owns the world, so world APIs that reach for
  // GetGameInstance (SetGameMode, subsystems) work.
  FScopedWorld() {
    GameInstance = NewObject<UGameInstance>(GEngine);
    GameInstance->InitializeStandalone();
    World = GameInstance->GetWorld();
    World->InitializeActorsForPlay(FURL());
  }

  ~FScopedWorld() {
    if (GameInstance) {
      GameInstance->Shutdown();
    }
    if (World) {
      GEngine->DestroyWorldContext(World);
      World->DestroyWorld(false);
    }
  }
};

} // namespace RedwoodZoneTravelTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelAbortTransferringTest,
  "Redwood.ZoneTravel.AbortTransferringClearsTheFlag",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelAbortTransferringTest::RunTest(
  const FString &Parameters
) {
  RedwoodZoneTravelTest::FScopedWorld Scoped;

  APlayerState *PlayerState = Scoped.World->SpawnActor<APlayerState>();
  if (!TestNotNull(TEXT("player state spawned"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *Component =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  Component->RegisterComponent();

  TestFalse(TEXT("not transferring initially"), Component->bTransferring);
  Component->InitTransferring();
  TestTrue(TEXT("InitTransferring latches the flag"), Component->bTransferring);
  Component->AbortTransferring();
  TestFalse(
    TEXT("AbortTransferring rolls the flag back"), Component->bTransferring
  );

  // Idempotent on an already-clear flag: the async error path can race a
  // second failed call.
  Component->AbortTransferring();
  TestFalse(TEXT("a second abort stays clear"), Component->bTransferring);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelSpawnClearanceTest,
  "Redwood.ZoneTravel.SpawnClearanceFollowsThePawnCapsule",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelSpawnClearanceTest::RunTest(const FString &Parameters) {
  RedwoodZoneTravelTest::FScopedWorld Scoped;

  ARedwoodZoneSpawn *Spawn = Scoped.World->SpawnActor<ARedwoodZoneSpawn>();
  if (!TestNotNull(TEXT("zone spawn spawned"), Spawn)) {
    return false;
  }

  // No game mode: the upstream 100-unit lift stays.
  TestEqual(
    TEXT("legacy clearance without a game mode"),
    Spawn->GetSpawnGroundClearance(),
    ARedwoodZoneSpawn::LegacySpawnGroundClearance
  );

  // With a Character default pawn: capsule half-height plus the margin.
  // SetGameMode is the only public way to give a bare world an
  // AuthorityGameMode; override its pawn class to the one under test.
  Scoped.World->SetGameMode(FURL());
  AGameModeBase *GameMode = Scoped.World->GetAuthGameMode();
  if (!TestNotNull(TEXT("world game mode set"), GameMode)) {
    return false;
  }
  GameMode->DefaultPawnClass = ACharacter::StaticClass();

  const ACharacter *PawnDefault =
    Cast<ACharacter>(ACharacter::StaticClass()->GetDefaultObject());
  const float Expected =
    PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
    ARedwoodZoneSpawn::SpawnClearanceMargin;
  TestEqual(
    TEXT("capsule-aware clearance with a Character pawn"),
    Spawn->GetSpawnGroundClearance(),
    Expected
  );

  // GetSpawnTransform applies the clearance above its trace result; with no
  // geometry the trace misses and the actor's own Z is the base.
  const FVector SpawnLocation(0.0f, 0.0f, 300.0f);
  Spawn->SetActorLocation(SpawnLocation);
  const FTransform Result = Spawn->GetSpawnTransform();
  TestEqual(
    TEXT("transform lifts by the clearance"),
    static_cast<float>(Result.GetLocation().Z),
    static_cast<float>(SpawnLocation.Z) + Expected,
    0.1f
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelPawnSpawnRecoveryTest,
  "Redwood.ZoneTravel.FailedPawnSpawnRecovers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelPawnSpawnRecoveryTest::RunTest(
  const FString &Parameters
) {
  RedwoodZoneTravelTest::FScopedWorld Scoped;

  AGameModeBase *GameMode = Scoped.World->SpawnActor<AGameModeBase>();
  // AController is abstract; a PlayerController stands in for the arriving
  // player.
  AController *Controller = Scoped.World->SpawnActor<APlayerController>();
  if (!TestNotNull(TEXT("game mode spawned"), GameMode) ||
      !TestNotNull(TEXT("controller spawned"), Controller)) {
    return false;
  }
  GameMode->DefaultPawnClass = ACharacter::StaticClass();

  URedwoodGameModeComponent *Component =
    NewObject<URedwoodGameModeComponent>(GameMode);
  Component->RegisterComponent();

  const FTransform FailedTransform(FVector(0.0f, 0.0f, 50.0f));
  APawn *Pawn =
    Component->RetryFailedPawnSpawn(GameMode, Controller, FailedTransform);
  if (!TestNotNull(TEXT("recovery produced a pawn"), Pawn)) {
    return false;
  }

  // Attempt 1 lifts by the capsule half-height.
  const ACharacter *PawnDefault =
    Cast<ACharacter>(ACharacter::StaticClass()->GetDefaultObject());
  const float ExpectedLift =
    PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
  TestEqual(
    TEXT("recovered pawn sits half a capsule higher"),
    static_cast<float>(Pawn->GetActorLocation().Z),
    static_cast<float>(FailedTransform.GetLocation().Z) + ExpectedLift,
    1.0f
  );

  // A null pawn class cannot recover; the caller keeps upstream's null.
  GameMode->DefaultPawnClass = nullptr;
  TestNull(
    TEXT("no pawn class returns null"),
    Component->RetryFailedPawnSpawn(GameMode, Controller, FailedTransform)
  );
  return true;
}

#endif // WITH_AUTOMATION_WORKER
