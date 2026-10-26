// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins the plugin side
// of the account reentry fix:
//   1. The optional "reentryHeldCharacterId" field of the player-auth reply is stored on the
//      player state BEFORE OnRedwoodCharacterUpdated fires, and an absent field clears it.
//   2. The late-auth release decision (a reply that arrives after its connection left).
//   3. FRedwoodPendingOperations, the registry behind the async part of
//      FlushDetachedCharacterData: exactly-once settlement on the last ack, on a dropped socket
//      or deadline (no release), and ignoring late acks.
//   4. FlushDetachedCharacterData runs its OnSettled callback exactly once on every exit that
//      needs no live sidecar. The wiring of the registry to a real sidecar (the OnDisconnectedCallback
//      hook, the deadline timer, and the sidecar-down and nothing-dirty exits) needs sidecar
//      coverage that this file does not have; no UE/backend integration run exercises it yet.
//   5. A detached item retry cannot run after its operation was abandoned; live and tick retries
//      still run, and the older batch's acknowledgment cannot clear a newer mutation.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodPendingOperations.h"
#include "RedwoodGameModeComponent.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "ReentryAdmissionListener.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

// Named, not anonymous: test files share a translation unit under the unity build.
namespace RedwoodReentryAdmissionTest {
  const FString TestCharacterId = TEXT("redwood-test-reentry-character");
  const FString TestPlayerId = TEXT("redwood-test-reentry-player");
  const FString TestHeldCharacterId = TEXT("redwood-test-reentry-held");

  // A PIE world takes the offline path while bUseBackendInPIE is off (the default).
  struct FOfflineWorld {
    UWorld *World = nullptr;
    UGameInstance *GameInstance = nullptr;

    FOfflineWorld() {
      World = UWorld::CreateWorld(
        EWorldType::PIE, false, TEXT("RedwoodReentryAdmissionWorld")
      );
      if (!World) {
        return;
      }
      FWorldContext &WorldContext =
        GEngine->CreateNewWorldContext(EWorldType::PIE);
      WorldContext.SetCurrentWorld(World);
      GameInstance = NewObject<UGameInstance>(GEngine);
      WorldContext.OwningGameInstance = GameInstance;
      World->SetGameInstance(GameInstance);
      GameInstance->OnWorldChanged(nullptr, World);
    }

    ~FOfflineWorld() {
      if (!World) {
        return;
      }
      GameInstance->OnWorldChanged(World, nullptr);
      GEngine->DestroyWorldContext(World);
      World->DestroyWorld(false);
    }
  };

  // The offline flush writes <Saved>/Persistence/Characters/<id>.json. Restores a file that was
  // there before, or deletes the one the test created, so no JSON is left behind.
  struct FCharacterFileRestorer {
    FString FilePath;
    TArray<uint8> OriginalBytes;
    bool bHadFile = false;

    explicit FCharacterFileRestorer(const FString &CharacterId) {
      FilePath = FPaths::ProjectSavedDir() / TEXT("Persistence") /
        TEXT("Characters") / (CharacterId + TEXT(".json"));
      FPaths::NormalizeFilename(FilePath);
      bHadFile = FFileHelper::LoadFileToArray(OriginalBytes, *FilePath);
    }

    ~FCharacterFileRestorer() {
      if (bHadFile) {
        FFileHelper::SaveArrayToFile(OriginalBytes, *FilePath);
      } else {
        IFileManager::Get().Delete(*FilePath, false, true, true);
      }
    }
  };

  TSharedPtr<FJsonObject> MakeAuthReply(const FString &HeldCharacterId) {
    TSharedPtr<FJsonObject> Character = MakeShareable(new FJsonObject);
    Character->SetStringField(TEXT("id"), TestCharacterId);
    Character->SetStringField(TEXT("playerId"), TestPlayerId);
    Character->SetStringField(TEXT("name"), TEXT("Tester"));
    Character->SetStringField(TEXT("createdAt"), TEXT("2026-01-01T00:00:00.000Z"));
    Character->SetStringField(TEXT("updatedAt"), TEXT("2026-01-01T00:00:00.000Z"));

    TSharedPtr<FJsonObject> Player = MakeShareable(new FJsonObject);
    Player->SetStringField(TEXT("id"), TestPlayerId);
    Player->SetStringField(TEXT("nickname"), TEXT("Tester"));

    TSharedPtr<FJsonObject> Reply = MakeShareable(new FJsonObject);
    Reply->SetStringField(TEXT("error"), TEXT(""));
    Reply->SetObjectField(TEXT("character"), Character);
    Reply->SetObjectField(TEXT("player"), Player);
    if (!HeldCharacterId.IsEmpty()) {
      Reply->SetStringField(TEXT("reentryHeldCharacterId"), HeldCharacterId);
    }
    return Reply;
  }

  URedwoodPlayerStateComponent *
    AddStateComponent(APlayerState *PlayerState, const FString &CharacterId) {
    URedwoodPlayerStateComponent *Component =
      NewObject<URedwoodPlayerStateComponent>(PlayerState);
    Component->RegisterComponent();
    Component->RedwoodCharacter.Id = CharacterId;
    return Component;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodReentryHeldIdParsedBeforeBroadcastTest,
  "Redwood.Reentry.HeldIdStoredBeforeCharacterBroadcast",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodReentryHeldIdParsedBeforeBroadcastTest::RunTest(
  const FString &Parameters
) {
  namespace Fixture = RedwoodReentryAdmissionTest;
  Fixture::FOfflineWorld TestWorld;
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }
  APlayerState *PlayerState = TestWorld.World->SpawnActor<APlayerState>();
  if (!TestNotNull(TEXT("Player state spawned"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *Component =
    Fixture::AddStateComponent(PlayerState, FString());
  TStrongObjectPtr<URedwoodReentryAuthListener> Listener(
    NewObject<URedwoodReentryAuthListener>()
  );
  Listener->Watch(Component);

  URedwoodGameModeComponent::ApplyAuthSuccessToPlayerState(
    Component, Fixture::MakeAuthReply(Fixture::TestHeldCharacterId)
  );
  TestEqual(TEXT("The field is stored"), Component->ReentryHeldCharacterId, Fixture::TestHeldCharacterId);
  TestEqual(TEXT("One character broadcast"), Listener->BroadcastCount, 1);
  TestEqual(
    TEXT("The listener already saw the field"),
    Listener->HeldIdAtBroadcast,
    Fixture::TestHeldCharacterId
  );
  TestEqual(
    TEXT("The character was applied"), Component->RedwoodCharacter.Id, Fixture::TestCharacterId
  );
  TestTrue(TEXT("The player is server ready"), Component->bServerReady);

  // A later reply without the field must not keep the old hold.
  URedwoodGameModeComponent::ApplyAuthSuccessToPlayerState(
    Component, Fixture::MakeAuthReply(FString())
  );
  TestTrue(TEXT("An absent field clears the hold"), Component->ReentryHeldCharacterId.IsEmpty());
  TestTrue(TEXT("The listener saw it clear"), Listener->HeldIdAtBroadcast.IsEmpty());
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodLateAuthReleaseDecisionTest,
  "Redwood.Reentry.LateAuthReleaseDecision",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodLateAuthReleaseDecisionTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodReentryAdmissionTest;
  Fixture::FOfflineWorld TestWorld;
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }
  APlayerState *Other = TestWorld.World->SpawnActor<APlayerState>();
  APlayerState *Owner = TestWorld.World->SpawnActor<APlayerState>();
  if (!TestNotNull(TEXT("Player states spawned"), Other) ||
      !TestNotNull(TEXT("Player states spawned"), Owner)) {
    return false;
  }
  Fixture::AddStateComponent(Other, TEXT("some-other-character"));
  Fixture::AddStateComponent(Owner, Fixture::TestCharacterId);

  int32 GateCalls = 0;
  FString GatePlayerId;
  FString GateCharacterId;
  FRedwoodShouldReleaseDepartedAuth Allow;
  Allow.BindLambda([&](const FString &PlayerId, const FString &CharacterId) {
    ++GateCalls;
    GatePlayerId = PlayerId;
    GateCharacterId = CharacterId;
    return true;
  });

  TestFalse(
    TEXT("Unbound gate never releases"),
    URedwoodGameModeComponent::ShouldReleaseLateAuth(
      FRedwoodShouldReleaseDepartedAuth(), {Other}, Fixture::TestPlayerId, Fixture::TestCharacterId
    )
  );
  TestTrue(
    TEXT("Allowed and unowned releases"),
    URedwoodGameModeComponent::ShouldReleaseLateAuth(
      Allow, {Other}, Fixture::TestPlayerId, Fixture::TestCharacterId
    )
  );
  TestEqual(TEXT("Gate got the reply player id"), GatePlayerId, Fixture::TestPlayerId);
  TestEqual(TEXT("Gate got the reply character id"), GateCharacterId, Fixture::TestCharacterId);

  GateCalls = 0;
  TestFalse(
    TEXT("A live player state with the character keeps the binding"),
    URedwoodGameModeComponent::ShouldReleaseLateAuth(
      Allow, {Other, Owner}, Fixture::TestPlayerId, Fixture::TestCharacterId
    )
  );
  TestEqual(TEXT("The gate is not asked when a player owns the character"), GateCalls, 0);

  FRedwoodShouldReleaseDepartedAuth Deny;
  Deny.BindLambda([](const FString &, const FString &) { return false; });
  TestFalse(
    TEXT("A refusing gate does not release"),
    URedwoodGameModeComponent::ShouldReleaseLateAuth(
      Deny, {}, Fixture::TestPlayerId, Fixture::TestCharacterId
    )
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodFlushDetachedSettlesOnceTest,
  "Redwood.Reentry.FlushDetachedSettlesExactlyOnce",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodFlushDetachedSettlesOnceTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodReentryAdmissionTest;
  Fixture::FCharacterFileRestorer SavedFileGuard(Fixture::TestCharacterId);
  Fixture::FOfflineWorld TestWorld;
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }
  URedwoodServerGameSubsystem *Subsystem =
    NewObject<URedwoodServerGameSubsystem>(TestWorld.GameInstance);

  int32 Settled = 0;
  auto Count = [&Settled]() { ++Settled; };

  // Empty id.
  Subsystem->FlushDetachedCharacterData(nullptr, FString(), Fixture::TestPlayerId, true, Count);
  TestEqual(TEXT("Empty id settles once"), Settled, 1);

  // No pawn.
  Settled = 0;
  Subsystem->FlushDetachedCharacterData(
    nullptr, Fixture::TestCharacterId, Fixture::TestPlayerId, true, Count
  );
  TestEqual(TEXT("Missing pawn settles once"), Settled, 1);

  // Pawn without the character component.
  APawn *Pawn = TestWorld.World->SpawnActor<APawn>();
  if (!TestNotNull(TEXT("Pawn spawned"), Pawn)) {
    return false;
  }
  Settled = 0;
  Subsystem->FlushDetachedCharacterData(
    Pawn, Fixture::TestCharacterId, Fixture::TestPlayerId, true, Count
  );
  TestEqual(TEXT("Missing component settles once"), Settled, 1);

  // Component carrying another character.
  URedwoodCharacterComponent *CharacterComponent =
    NewObject<URedwoodCharacterComponent>(Pawn);
  CharacterComponent->RegisterComponent();
  CharacterComponent->RedwoodCharacterId = TEXT("someone-else");
  Settled = 0;
  Subsystem->FlushDetachedCharacterData(
    Pawn, Fixture::TestCharacterId, Fixture::TestPlayerId, true, Count
  );
  TestEqual(TEXT("Id mismatch settles once"), Settled, 1);

  // Offline save: every group off, so only the identity fields are written.
  CharacterComponent->RedwoodCharacterId = Fixture::TestCharacterId;
  CharacterComponent->bUseCharacterCreatorData = false;
  CharacterComponent->bUseMetadata = false;
  CharacterComponent->bUseEquippedInventory = false;
  CharacterComponent->bUseNonequippedInventory = false;
  CharacterComponent->bUseItems = false;
  CharacterComponent->bUseProgress = false;
  CharacterComponent->bUseData = false;
  CharacterComponent->bUseAbilitySystem = false;
  Settled = 0;
  Subsystem->FlushDetachedCharacterData(
    Pawn, Fixture::TestCharacterId, Fixture::TestPlayerId, true, Count
  );
  TestEqual(TEXT("Offline save with release settles once"), Settled, 1);

  Settled = 0;
  Subsystem->FlushDetachedCharacterData(
    Pawn, Fixture::TestCharacterId, Fixture::TestPlayerId, false, Count
  );
  TestEqual(TEXT("Offline save without release settles once"), Settled, 1);

  // The callback-less overload still works.
  Subsystem->FlushDetachedCharacterData(
    Pawn, Fixture::TestCharacterId, Fixture::TestPlayerId, true
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodDetachedFlushSettlesOnLastAckTest,
  "Redwood.Reentry.SettlementReleasesThenSettlesOnLastAck",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodDetachedFlushSettlesOnLastAckTest::RunTest(
  const FString &Parameters
) {
  FRedwoodPendingOperations Registry;
  TArray<FString> Order;
  const uint64 Id = Registry.Begin(2, [&Order](bool bDelivered) {
    if (bDelivered) {
      Order.Add(TEXT("release"));
    }
    Order.Add(TEXT("settled"));
  });
  TFunction<void()> Ack = Registry.MakeWriteSettled(Id);

  Ack();
  TestEqual(TEXT("One ack of two settles nothing"), Order.Num(), 0);
  Ack();
  TestEqual(TEXT("Release then settled"), FString::Join(Order, TEXT(",")), FString(TEXT("release,settled")));
  Ack();
  Ack();
  TestEqual(TEXT("Extra acks run nothing again"), Order.Num(), 2);
  TestEqual(TEXT("Nothing pending"), Registry.Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodDetachedFlushAbandonTest,
  "Redwood.Reentry.SettlementAbandonedWithoutReleaseIgnoresLateAck",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodDetachedFlushAbandonTest::RunTest(const FString &Parameters) {
  FRedwoodPendingOperations Registry;
  int32 Releases = 0;
  int32 SettledA = 0;
  int32 SettledB = 0;
  const uint64 IdA = Registry.Begin(2, [&Releases, &SettledA](bool bDelivered) {
    if (bDelivered) {
      ++Releases;
    }
    ++SettledA;
  });
  const uint64 IdB = Registry.Begin(1, [&Releases, &SettledB](bool bDelivered) {
    if (bDelivered) {
      ++Releases;
    }
    ++SettledB;
  });
  TFunction<void()> AckA = Registry.MakeWriteSettled(IdA);
  TFunction<void()> AckB = Registry.MakeWriteSettled(IdB);

  // Exercise the callback bound to the deadline timer: B stays pending.
  TFunction<void()> DeadlineA = Registry.MakeAbandon(IdA);
  AckA();
  DeadlineA();
  TestEqual(TEXT("A settled once"), SettledA, 1);
  TestEqual(TEXT("No release for an abandoned operation"), Releases, 0);
  TestEqual(TEXT("B still pending"), Registry.Num(), 1);
  DeadlineA();
  TestEqual(TEXT("A abandoned twice settles once"), SettledA, 1);

  // The late ack of A, as after a replacement session took over, must emit nothing.
  AckA();
  TestEqual(TEXT("Late ack emits no release"), Releases, 0);
  TestEqual(TEXT("Late ack settles nothing again"), SettledA, 1);

  // Socket dropped: B settles without release, then its own late ack is ignored.
  Registry.AbandonAll();
  TestEqual(TEXT("B settled once on disconnect"), SettledB, 1);
  AckB();
  TestEqual(TEXT("Late ack of B emits no release"), Releases, 0);
  TestEqual(TEXT("B settled exactly once"), SettledB, 1);
  TestEqual(TEXT("Nothing pending"), Registry.Num(), 0);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodDetachedFlushReentrantSettleTest,
  "Redwood.Reentry.SettlementHandlerMayStartNewOperation",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodDetachedFlushReentrantSettleTest::RunTest(
  const FString &Parameters
) {
  FRedwoodPendingOperations Registry;
  int32 SettledSecond = 0;
  Registry.Begin(1, [&](bool) {
    // A disconnect handler that retries must stay pending for the next connection.
    Registry.Begin(1, [&SettledSecond](bool) { ++SettledSecond; });
  });
  Registry.AbandonAll();
  TestEqual(TEXT("The new operation stays pending"), Registry.Num(), 1);
  TestEqual(TEXT("The new operation has not settled"), SettledSecond, 0);
  Registry.AbandonAll();
  TestEqual(TEXT("It settles on the next drop"), SettledSecond, 1);
  return true;
}

// The player-auth request uses the same registry: a reply that comes after the request was
// abandoned must be seen as late (IsPending false), the abandon hook runs only on abandon, and the
// settled hook runs exactly once on either path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodAuthRequestLifetimeTest,
  "Redwood.Reentry.AuthRequestAbandonHookAndLateReply",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodAuthRequestLifetimeTest::RunTest(const FString &Parameters) {
  FRedwoodPendingOperations Registry;
  int32 Abandoned = 0;
  int32 Settled = 0;
  auto Begin = [&]() {
    return Registry.Begin(1, [&Settled, &Abandoned](bool bDelivered) {
      if (!bDelivered) {
        ++Abandoned;
      }
      ++Settled;
    });
  };

  // Reply first: no abandon hook, one settled, and the request is no longer pending.
  const uint64 Answered = Begin();
  TFunction<bool()> AnsweredPending = Registry.MakeIsPending(Answered);
  TestTrue(TEXT("Pending before the reply"), AnsweredPending());
  Registry.MakeWriteSettled(Answered)();
  TestFalse(TEXT("Not pending after the reply"), AnsweredPending());
  TestEqual(TEXT("Reply: no abandon hook"), Abandoned, 0);
  TestEqual(TEXT("Reply: settled once"), Settled, 1);
  Registry.Abandon(Answered);
  TestEqual(TEXT("A deadline after the reply does nothing"), Abandoned + Settled, 1);

  // Deadline first: abandon hook then settled, and the late reply sees it was abandoned.
  const uint64 Lost = Begin();
  TFunction<bool()> LostPending = Registry.MakeIsPending(Lost);
  TFunction<void()> LateReply = Registry.MakeWriteSettled(Lost);
  Registry.Abandon(Lost);
  TestEqual(TEXT("Deadline: abandon hook once"), Abandoned, 1);
  TestEqual(TEXT("Deadline: settled once more"), Settled, 2);
  TestFalse(TEXT("The late reply sees the request as gone"), LostPending());
  LateReply();
  TestEqual(TEXT("Late reply: abandon hook not repeated"), Abandoned, 1);
  TestEqual(TEXT("Late reply: not settled again"), Settled, 2);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodDetachedItemRetryLifetimeTest,
  "Redwood.Reentry.DetachedItemRetryHonorsAbandonment",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodDetachedItemRetryLifetimeTest::RunTest(const FString &Parameters) {
  struct FRetryCase {
    const TCHAR *Name;
    bool bGuarded;
    bool bAbandon;
  };
  const FRetryCase Cases[] = {
    {TEXT("Abandoned detached save"), true, true},
    {TEXT("Live detached save"), true, false},
    {TEXT("Ordinary tick retry"), false, true}
  };
  const FString ItemId = TEXT("detached-retry-item");

  for (const FRetryCase &Case : Cases) {
    const FString Prefix = FString(Case.Name) + TEXT(": ");
    TStrongObjectPtr<URedwoodCharacterComponent> Component(
      NewObject<URedwoodCharacterComponent>()
    );
    Component->bUseItems = true;
    Component->MarkItemsDirty({ItemId});
    const TArray<TPair<FString, uint64>> SentUpserts = {
      {ItemId, Component->GetDirtyItemGenerations().FindChecked(ItemId)}
    };
    int64 BatchSeq = 0;
    if (!TestTrue(Prefix + TEXT("an earlier batch is in flight"),
        Component->TryBeginItemFlush(BatchSeq))) {
      return false;
    }
    // The earlier batch cannot acknowledge the mutation waiting behind it.
    Component->MarkItemsDirty({ItemId});

    FRedwoodPendingOperations Operations;
    int32 Settled = 0;
    bool bDelivered = false;
    const uint64 OperationId = Operations.Begin(
      1, [&Settled, &bDelivered](bool bWasDelivered) {
        ++Settled;
        bDelivered = bWasDelivered;
      }
    );
    TFunction<void()> SettleRetry = Operations.MakeWriteSettled(OperationId);
    TFunction<bool()> ShouldRun;
    if (Case.bGuarded) {
      ShouldRun = Operations.MakeIsPending(OperationId);
    }
    int32 RetryCount = 0;
    Component->SetPendingItemFlushRetry(
      [&RetryCount, SettleRetry]() {
        ++RetryCount;
        SettleRetry();
      },
      MoveTemp(ShouldRun)
    );
    TestTrue(Prefix + TEXT("the retry is parked"), Component->HasPendingItemFlushRetry());
    if (Case.bAbandon) {
      Operations.Abandon(OperationId);
    }

    Component->CompleteItemFlush(FString(), BatchSeq, SentUpserts, {});
    const int32 ExpectedRetries = Case.bGuarded && Case.bAbandon ? 0 : 1;
    TestEqual(Prefix + TEXT("only a permitted retry runs"), RetryCount, ExpectedRetries);
    TestFalse(Prefix + TEXT("the parked slot is cleared"), Component->HasPendingItemFlushRetry());
    TestTrue(Prefix + TEXT("the newer item mutation stays dirty"),
      Component->GetDirtyItemGenerations().Contains(ItemId));
    TestEqual(Prefix + TEXT("the operation settles once"), Settled, 1);
    TestEqual(Prefix + TEXT("abandonment does not become delivery"), bDelivered, !Case.bAbandon);
    Component->CompleteItemFlush(FString(), BatchSeq, SentUpserts, {});
    TestEqual(Prefix + TEXT("another acknowledgment cannot repeat the retry"), RetryCount, ExpectedRetries);
  }
  return true;
}
