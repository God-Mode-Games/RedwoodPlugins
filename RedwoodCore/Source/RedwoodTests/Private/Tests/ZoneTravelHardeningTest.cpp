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
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
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

  // The server-side events the game binds to put a loading screen up and
  // take it down again.
  int32 StartCount = 0;
  int32 AbortCount = 0;
  FString AbortError;
  FString AbortReason;
  Component->OnTransferringStartedServer.AddLambda([&StartCount]() {
    ++StartCount;
  });
  Component->OnTransferAbortedServer.AddLambda(
    [&AbortCount, &AbortError, &AbortReason](
      const FString &Error, const FString &Reason
    ) {
      ++AbortCount;
      AbortError = Error;
      AbortReason = Reason;
    }
  );

  // The owning client gets the same pair through the client RPC. This world
  // is standalone, so the RPC runs locally and the listener sees it.
  URedwoodTransferAbortListener *Listener =
    NewObject<URedwoodTransferAbortListener>(Component);
  Component->OnTransferAborted.AddDynamic(
    Listener, &URedwoodTransferAbortListener::OnTransferAborted
  );

  TestFalse(TEXT("not transferring initially"), Component->bTransferring);
  Component->InitTransferring();
  TestTrue(TEXT("InitTransferring latches the flag"), Component->bTransferring);
  TestEqual(TEXT("the server hears the transfer start"), StartCount, 1);

  // Both the error text and the reason token must survive the whole chain;
  // the game maps the token to its own failure type.
  Component->AbortTransferring(TEXT("zone is full"), TEXT("ZoneNotConfigured"));
  TestFalse(
    TEXT("AbortTransferring rolls the flag back"), Component->bTransferring
  );
  TestEqual(TEXT("the server hears the abort"), AbortCount, 1);
  TestEqual(
    TEXT("the abort carries the error"), AbortError, TEXT("zone is full")
  );
  TestEqual(
    TEXT("the abort carries the reason"),
    AbortReason,
    TEXT("ZoneNotConfigured")
  );
  TestEqual(TEXT("the client hears the abort"), Listener->Count, 1);
  TestEqual(
    TEXT("the client gets the error"), Listener->Error, TEXT("zone is full")
  );
  TestEqual(
    TEXT("the client gets the reason"),
    Listener->Reason,
    TEXT("ZoneNotConfigured")
  );

  // Idempotent on an already-clear flag: two failure paths can report the
  // same transfer, and the second one must broadcast nothing.
  Component->AbortTransferring(TEXT("zone is full"), TEXT("ZoneNotConfigured"));
  TestFalse(TEXT("a second abort stays clear"), Component->bTransferring);
  TestEqual(TEXT("a second abort broadcasts nothing"), AbortCount, 1);
  TestEqual(TEXT("the client hears nothing either"), Listener->Count, 1);

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

  APlayerState *PlayerState = Scoped.World->SpawnActor<APlayerState>();
  if (!TestNotNull(TEXT("player state spawned"), PlayerState)) {
    return false;
  }
  PlayerController->PlayerState = PlayerState;

  URedwoodPlayerStateComponent *Component =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  Component->RegisterComponent();

  TWeakObjectPtr<APlayerController> WeakPlayerController(PlayerController);

  Component->InitTransferring();

  // No error at all: the transfer goes on.
  TSharedPtr<FJsonObject> Success = MakeShareable(new FJsonObject);
  Success->SetStringField(TEXT("error"), TEXT(""));
  Subsystem->HandleTransferZoneResponse(Success, WeakPlayerController);
  TestTrue(TEXT("no error keeps the transfer"), Component->bTransferring);

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
  int32 AbortCount = 0;
  FString AbortError;
  FString AbortReason;
  Component->OnTransferAbortedServer.AddLambda(
    [&AbortCount, &AbortError, &AbortReason](
      const FString &Error, const FString &Reason
    ) {
      ++AbortCount;
      AbortError = Error;
      AbortReason = Reason;
    }
  );
  TSharedPtr<FJsonObject> Safe = MakeShareable(new FJsonObject);
  Safe->SetStringField(TEXT("error"), TEXT("zone is full"));
  Safe->SetBoolField(TEXT("ambiguous"), false);
  Subsystem->HandleTransferZoneResponse(Safe, WeakPlayerController);
  TestFalse(
    TEXT("an explicit false rolls the flag back"), Component->bTransferring
  );
  TestEqual(TEXT("the rollback happened once"), AbortCount, 1);
  TestEqual(
    TEXT("the rollback carries the error"), AbortError, TEXT("zone is full")
  );
  TestEqual(
    TEXT("the rollback names the realm as the source"),
    AbortReason,
    TEXT("realm-rejected")
  );

  // The same answer again, with the player no longer transferring: a stale
  // answer must not fire the rollback events a second time.
  Subsystem->HandleTransferZoneResponse(Safe, WeakPlayerController);
  TestEqual(TEXT("a stale answer does not roll back again"), AbortCount, 1);
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
