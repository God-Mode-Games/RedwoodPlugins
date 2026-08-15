// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added — no upstream counterpart.
// Pins the zone-travel hardening contracts (HollowedOath#1752 items 4, 7, 8):
//   1. URedwoodPlayerStateComponent::AbortTransferring clears the
//      bTransferring latch that InitTransferring sets before the
//      TravelPlayerToZone* sidecar checks, and both tell the server through
//      OnTransferringStartedServer / OnTransferAbortedServer.
//   2. ARedwoodZoneSpawn::GetSpawnGroundClearance lifts by the default
//      pawn's capsule half-height plus a margin when the game mode exposes
//      an ACharacter pawn — never below the upstream 100 units — and keeps
//      the upstream 100 units otherwise.
//   3. URedwoodGameModeComponent::RetryFailedPawnSpawn recovers a failed
//      default-pawn spawn instead of leaving the player a pawnless
//      spectator.
//   4. URedwoodGameModeComponent::ResolveFallbackArrivalTransform refuses a
//      map with no arrival point — never the world-origin AWorldSettings
//      actor that a bare FindPlayerStart returns.
//   5. URedwoodServerGameSubsystem::HandleTransferZoneResponse rolls the
//      transfer back ONLY when the sidecar marks the error "ambiguous":
//      false, and only while the player is still transferring. An ambiguous
//      error, or an answer with no such field (an older sidecar), kicks.
//   6. URedwoodPlayerStateComponent::MatchesActiveTransfer drops a failure
//      report that names a different transfer, but accepts one when either
//      side has no id (an older backend, or a report that overtakes the
//      answer that carries the id).
//   7. URedwoodServerGameSubsystem::HandleTransferFailedEvent rolls the
//      transfer back for the character the realm names, and only when that
//      player is still transferring and the ids agree.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "RedwoodZoneSpawn.h"
#include "TransferAbortListener.h"

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

  // Spawns a player state carrying a Redwood player state component, which
  // every transfer test needs. Returns null when the spawn fails; the
  // caller asserts on the result.
  URedwoodPlayerStateComponent *SpawnPlayerStateComponent(
    APlayerState *&OutPlayerState
  ) {
    OutPlayerState = World->SpawnActor<APlayerState>();

    if (!OutPlayerState) {
      return nullptr;
    }

    URedwoodPlayerStateComponent *Component =
      NewObject<URedwoodPlayerStateComponent>(OutPlayerState);
    Component->RegisterComponent();
    return Component;
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

  APlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *Component =
    Scoped.SpawnPlayerStateComponent(PlayerState);
  if (!TestNotNull(TEXT("player state component spawned"), Component)) {
    return false;
  }

  // The events the game binds to put a loading screen up and take it down
  // again. The world is standalone, so the client RPC runs locally and the
  // listener sees both sides.
  int32 StartCount = 0;
  Component->OnTransferringStartedServer.AddLambda([&StartCount]() {
    ++StartCount;
  });
  URedwoodTransferAbortListener *Listener =
    NewObject<URedwoodTransferAbortListener>(Component);
  Listener->Watch(Component);

  TestFalse(TEXT("not transferring initially"), Component->bTransferring);
  Component->InitTransferring();
  TestTrue(TEXT("InitTransferring latches the flag"), Component->bTransferring);
  TestEqual(TEXT("the server hears the transfer start"), StartCount, 1);

  // Both the error text and the reason token must survive the whole chain;
  // the game maps the token to its own failure type.
  Component->AbortTransferring(
    TEXT("zone is full"), TEXT("zone-not-configured")
  );
  TestFalse(
    TEXT("AbortTransferring rolls the flag back"), Component->bTransferring
  );
  TestEqual(TEXT("the server hears the abort"), Listener->ServerCount, 1);
  TestEqual(
    TEXT("the abort carries the error"),
    Listener->ServerError,
    TEXT("zone is full")
  );
  TestEqual(
    TEXT("the abort carries the reason"),
    Listener->ServerReason,
    TEXT("zone-not-configured")
  );
  TestEqual(TEXT("the client hears the abort"), Listener->ClientCount, 1);
  TestEqual(
    TEXT("the client gets the error"),
    Listener->ClientError,
    TEXT("zone is full")
  );
  TestEqual(
    TEXT("the client gets the reason"),
    Listener->ClientReason,
    TEXT("zone-not-configured")
  );

  // Idempotent on an already-clear flag: two failure paths can report the
  // same transfer, and the second one must broadcast nothing.
  Component->AbortTransferring(
    TEXT("zone is full"), TEXT("zone-not-configured")
  );
  TestFalse(TEXT("a second abort stays clear"), Component->bTransferring);
  TestEqual(TEXT("a second abort broadcasts nothing"), Listener->ServerCount, 1);
  TestEqual(TEXT("the client hears nothing either"), Listener->ClientCount, 1);

  // A repeated start still fires its events; that is upstream behaviour.
  Component->InitTransferring();
  Component->InitTransferring();
  TestEqual(TEXT("every start is announced"), StartCount, 3);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelTransferErrorTest,
  "Redwood.ZoneTravel.OnlyAnAmbiguousTransferErrorKicks",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelTransferErrorTest::RunTest(const FString &Parameters) {
  // Every failure branch logs at Error level on purpose: two kicks and two
  // rollback attempts below.
  AddExpectedError(
    TEXT("Failed to transfer player to new zone"),
    EAutomationExpectedErrorFlags::Contains,
    4
  );
  // The stale answer only warns; expect it in case the harness raises
  // warnings to errors.
  AddExpectedError(
    TEXT("not transferring"), EAutomationExpectedErrorFlags::Contains, 0
  );

  RedwoodZoneTravelTest::FScopedWorld Scoped;

  URedwoodServerGameSubsystem *Subsystem =
    Scoped.GameInstance->GetSubsystem<URedwoodServerGameSubsystem>();
  APlayerController *PlayerController =
    Scoped.World->SpawnActor<APlayerController>();
  if (!TestNotNull(TEXT("subsystem available"), Subsystem) ||
      !TestNotNull(TEXT("controller spawned"), PlayerController)) {
    return false;
  }

  APlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *Component =
    Scoped.SpawnPlayerStateComponent(PlayerState);
  if (!TestNotNull(TEXT("player state component spawned"), Component)) {
    return false;
  }
  PlayerController->PlayerState = PlayerState;

  TWeakObjectPtr<APlayerController> WeakPlayerController(PlayerController);

  Component->InitTransferring();

  // No error at all: the transfer goes on.
  TSharedPtr<FJsonObject> Success = MakeShareable(new FJsonObject);
  Success->SetStringField(TEXT("error"), TEXT(""));
  Success->SetStringField(TEXT("transferId"), TEXT("transfer-1"));
  Subsystem->HandleTransferZoneResponse(Success, WeakPlayerController);
  TestTrue(TEXT("no error keeps the transfer"), Component->bTransferring);
  TestEqual(
    TEXT("the answer's transfer id is kept"),
    Component->ActiveTransferId,
    TEXT("transfer-1")
  );

  // Ambiguous: the realm can hold the character already, so the flag stays
  // and the player is kicked instead (this world has no GameSession, so the
  // kick itself is a no-op here).
  TSharedPtr<FJsonObject> Ambiguous = MakeShareable(new FJsonObject);
  Ambiguous->SetStringField(TEXT("error"), TEXT("realm timed out"));
  Ambiguous->SetBoolField(TEXT("ambiguous"), true);
  Subsystem->HandleTransferZoneResponse(Ambiguous, WeakPlayerController);
  TestTrue(TEXT("an ambiguous error keeps the flag"), Component->bTransferring);

  // No "ambiguous" field at all: an old sidecar that does not send it. This
  // must behave as ambiguous, or a game server that runs against an older
  // backend can roll a transfer back that the realm already committed.
  TSharedPtr<FJsonObject> NoField = MakeShareable(new FJsonObject);
  NoField->SetStringField(TEXT("error"), TEXT("realm timed out"));
  Subsystem->HandleTransferZoneResponse(NoField, WeakPlayerController);
  TestTrue(
    TEXT("a missing ambiguous field keeps the flag"), Component->bTransferring
  );

  // An explicit false proves the transfer never started: roll back, do not
  // kick.
  URedwoodTransferAbortListener *Listener =
    NewObject<URedwoodTransferAbortListener>(Component);
  Listener->Watch(Component);

  TSharedPtr<FJsonObject> Safe = MakeShareable(new FJsonObject);
  Safe->SetStringField(TEXT("error"), TEXT("zone is full"));
  Safe->SetBoolField(TEXT("ambiguous"), false);
  Subsystem->HandleTransferZoneResponse(Safe, WeakPlayerController);
  TestFalse(
    TEXT("an explicit false rolls the flag back"), Component->bTransferring
  );
  TestEqual(TEXT("the rollback happened once"), Listener->ServerCount, 1);
  TestEqual(
    TEXT("the rollback carries the error"),
    Listener->ServerError,
    TEXT("zone is full")
  );
  TestEqual(
    TEXT("the rollback names the realm as the source"),
    Listener->ServerReason,
    TEXT("realm-rejected")
  );
  TestEqual(
    TEXT("the rollback clears the transfer id"),
    Component->ActiveTransferId,
    FString()
  );

  // The same answer again, with the player no longer transferring: a stale
  // answer must not fire the rollback events a second time.
  Subsystem->HandleTransferZoneResponse(Safe, WeakPlayerController);
  TestEqual(
    TEXT("a stale answer does not roll back again"), Listener->ServerCount, 1
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelTransferFailedEventTest,
  "Redwood.ZoneTravel.TheRealmsFailureReportRollsTheTransferBack",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelTransferFailedEventTest::RunTest(
  const FString &Parameters
) {
  // The wire name. The backend pins its own copy of this string, and the two
  // cannot share a constant, so a change on either side must break a test
  // rather than turn the rollback off in silence.
  TestEqual(
    TEXT("the transfer-failed event keeps its wire name"),
    FString(URedwoodServerGameSubsystem::TransferFailedEventName),
    TEXT("realm:servers:transfer-zone:transfer-failed")
  );

  // Every gate below logs a warning; expect each one in case the harness
  // raises warnings to errors.
  const EAutomationExpectedErrorFlags::MatchType Contains =
    EAutomationExpectedErrorFlags::Contains;
  AddExpectedError(TEXT("but the player is not transferring"), Contains, 0);
  AddExpectedError(TEXT("but no player on this server matches"), Contains, 0);
  AddExpectedError(TEXT("Ignoring a failed transfer report"), Contains, 0);
  AddExpectedError(TEXT("The realm could not transfer character"), Contains, 0);

  RedwoodZoneTravelTest::FScopedWorld Scoped;

  URedwoodServerGameSubsystem *Subsystem =
    Scoped.GameInstance->GetSubsystem<URedwoodServerGameSubsystem>();
  if (!TestNotNull(TEXT("subsystem available"), Subsystem)) {
    return false;
  }

  // The handler walks GameState->PlayerArray, so the world needs a game
  // state. AGameStateBase collects every APlayerState already in the world
  // when it spawns, so the spawn order does not matter.
  AGameStateBase *GameState = Scoped.World->SpawnActor<AGameStateBase>();
  APlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *Component =
    Scoped.SpawnPlayerStateComponent(PlayerState);
  if (!TestNotNull(TEXT("game state spawned"), GameState) ||
      !TestNotNull(TEXT("player state component spawned"), Component)) {
    return false;
  }
  if (!TestTrue(
        TEXT("the player state joined the game state"),
        GameState->PlayerArray.Contains(PlayerState)
      )) {
    return false;
  }
  Component->RedwoodCharacter.Id = TEXT("character-1");

  URedwoodTransferAbortListener *Listener =
    NewObject<URedwoodTransferAbortListener>(Component);
  Listener->Watch(Component);

  // Builds the payload the realm sends.
  auto MakeReport = [](const FString &CharacterId, const FString &TransferId) {
    TSharedPtr<FJsonObject> Report = MakeShareable(new FJsonObject);
    Report->SetStringField(TEXT("playerId"), TEXT("player-1"));
    Report->SetStringField(TEXT("characterId"), CharacterId);
    Report->SetStringField(TEXT("transferId"), TransferId);
    Report->SetStringField(TEXT("error"), TEXT("the zone did not start"));
    Report->SetStringField(TEXT("reason"), TEXT("zone-start-timeout"));
    return Report;
  };

  // Not transferring: the report is stale or wrong, so nothing fires.
  Subsystem->HandleTransferFailedEvent(
    MakeReport(TEXT("character-1"), TEXT("transfer-1"))
  );
  TestEqual(
    TEXT("a report for a player who is not transferring is ignored"),
    Listener->ServerCount,
    0
  );

  Component->InitTransferring();
  Component->ActiveTransferId = TEXT("transfer-1");

  // A report for another character on this server must not touch this one.
  Subsystem->HandleTransferFailedEvent(
    MakeReport(TEXT("character-2"), TEXT("transfer-1"))
  );
  TestEqual(
    TEXT("a report for another character is ignored"), Listener->ServerCount, 0
  );

  // A report that names an earlier transfer must not roll this one back.
  Subsystem->HandleTransferFailedEvent(
    MakeReport(TEXT("character-1"), TEXT("transfer-0"))
  );
  TestEqual(
    TEXT("a report for another transfer is ignored"), Listener->ServerCount, 0
  );
  TestTrue(TEXT("the transfer is still in flight"), Component->bTransferring);

  // The matching report aborts once and hands the backend's reason on.
  Subsystem->HandleTransferFailedEvent(
    MakeReport(TEXT("character-1"), TEXT("transfer-1"))
  );
  TestFalse(TEXT("the report rolls the flag back"), Component->bTransferring);
  TestEqual(TEXT("the abort fired once"), Listener->ServerCount, 1);
  TestEqual(
    TEXT("the abort carries the realm's error"),
    Listener->ServerError,
    TEXT("the zone did not start")
  );
  TestEqual(
    TEXT("the abort carries the realm's reason"),
    Listener->ServerReason,
    TEXT("zone-start-timeout")
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelTransferIdMatchTest,
  "Redwood.ZoneTravel.AFailureReportMustNameTheTransferInFlight",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelTransferIdMatchTest::RunTest(const FString &Parameters) {
  RedwoodZoneTravelTest::FScopedWorld Scoped;

  APlayerState *PlayerState = nullptr;
  URedwoodPlayerStateComponent *Component =
    Scoped.SpawnPlayerStateComponent(PlayerState);
  if (!TestNotNull(TEXT("player state component spawned"), Component)) {
    return false;
  }

  // Nothing named yet: every report matches, so a failure that arrives
  // before the answer still reaches the abort.
  TestTrue(
    TEXT("an unnamed transfer matches any report"),
    Component->MatchesActiveTransfer(TEXT("transfer-1"))
  );

  Component->InitTransferring();
  Component->ActiveTransferId = TEXT("transfer-1");

  TestTrue(
    TEXT("the same id matches"),
    Component->MatchesActiveTransfer(TEXT("transfer-1"))
  );
  TestFalse(
    TEXT("a different id does not match"),
    Component->MatchesActiveTransfer(TEXT("transfer-0"))
  );
  // Compatibility: an older backend sends no id, so a report with none must
  // still reach the abort.
  TestTrue(
    TEXT("a report with no id matches"),
    Component->MatchesActiveTransfer(TEXT(""))
  );

  // The abort clears the id, so no later report can match a transfer that
  // is over.
  Component->AbortTransferring(TEXT("zone is full"), TEXT("realm-rejected"));
  TestEqual(
    TEXT("the abort clears the transfer id"),
    Component->ActiveTransferId,
    FString()
  );
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

  // With a Character default pawn: capsule half-height plus the margin,
  // never below the upstream lift. SetGameMode is the only public way to
  // give a world an AuthorityGameMode (UWorld::AuthorityGameMode is
  // private); WHICH game mode class the host project's ini resolves does
  // not matter, because the pawn class is overridden right after.
  Scoped.World->SetGameMode(FURL());
  AGameModeBase *GameMode = Scoped.World->GetAuthGameMode();
  if (!TestNotNull(TEXT("world game mode set"), GameMode)) {
    return false;
  }
  GameMode->DefaultPawnClass = ACharacter::StaticClass();

  const ACharacter *PawnDefault =
    Cast<ACharacter>(ACharacter::StaticClass()->GetDefaultObject());
  const float Expected = FMath::Max(
    PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
      ARedwoodZoneSpawn::SpawnClearanceMargin,
    ARedwoodZoneSpawn::LegacySpawnGroundClearance
  );
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodZoneTravelFallbackArrivalTest,
  "Redwood.ZoneTravel.FallbackArrivalNeverUsesTheWorldOrigin",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodZoneTravelFallbackArrivalTest::RunTest(const FString &Parameters) {
  // FindPlayerStart never returns null: with no APlayerStart it returns the
  // AWorldSettings actor at the world origin. A force-spawn there writes
  // (0,0,0) into lastLocation and corrupts every later login, so the
  // fallback resolution must refuse it.
  RedwoodZoneTravelTest::FScopedWorld Scoped;

  AGameModeBase *GameMode = Scoped.World->SpawnActor<AGameModeBase>();
  AController *Controller = Scoped.World->SpawnActor<APlayerController>();
  if (!TestNotNull(TEXT("game mode spawned"), GameMode) ||
      !TestNotNull(TEXT("controller spawned"), Controller)) {
    return false;
  }
  URedwoodGameModeComponent *Component =
    NewObject<URedwoodGameModeComponent>(GameMode);
  Component->RegisterComponent();

  // No PlayerStart, no zone spawn: refuse, never the origin.
  FTransform Fallback;
  TestFalse(
    TEXT("a map with no arrival point resolves nothing"),
    Component->ResolveFallbackArrivalTransform(GameMode, Controller, Fallback)
  );

  // A zone spawn is an arrival point.
  const FVector ZoneSpawnLocation(1000.0f, 2000.0f, 300.0f);
  ARedwoodZoneSpawn *ZoneSpawn = Scoped.World->SpawnActor<ARedwoodZoneSpawn>(
    ARedwoodZoneSpawn::StaticClass(), ZoneSpawnLocation, FRotator::ZeroRotator
  );
  if (!TestNotNull(TEXT("zone spawn spawned"), ZoneSpawn)) {
    return false;
  }
  TestTrue(
    TEXT("the zone spawn resolves"),
    Component->ResolveFallbackArrivalTransform(GameMode, Controller, Fallback)
  );
  TestEqual(
    TEXT("the fallback is the zone spawn, not the origin (x)"),
    static_cast<float>(Fallback.GetLocation().X),
    static_cast<float>(ZoneSpawnLocation.X),
    1.0f
  );

  // A real APlayerStart outranks the zone spawn.
  const FVector StartLocation(-500.0f, 0.0f, 100.0f);
  APlayerStart *PlayerStart = Scoped.World->SpawnActor<APlayerStart>(
    APlayerStart::StaticClass(), StartLocation, FRotator::ZeroRotator
  );
  if (!TestNotNull(TEXT("player start spawned"), PlayerStart)) {
    return false;
  }
  TestTrue(
    TEXT("the player start resolves"),
    Component->ResolveFallbackArrivalTransform(GameMode, Controller, Fallback)
  );
  TestEqual(
    TEXT("the player start wins (x)"),
    static_cast<float>(Fallback.GetLocation().X),
    static_cast<float>(StartLocation.X),
    1.0f
  );
  return true;
}

#endif // WITH_AUTOMATION_WORKER
