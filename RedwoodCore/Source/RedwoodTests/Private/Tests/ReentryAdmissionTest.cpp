// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins the plugin side
// of the account reentry fix:
//   1. The optional "reentryHeldCharacterId" field of the player-auth reply is stored on the
//      player state BEFORE OnRedwoodCharacterUpdated fires, and an absent field clears it.
//   2. The late-auth release decision (a reply that arrives after its connection left).
//   3. FlushDetachedCharacterData runs its OnSettled callback exactly once on every exit that
//      needs no live sidecar. The async latch path (and the sidecar-down and nothing-dirty exits)
//      needs sidecar coverage that this file does not have; no UE/backend integration run
//      exercises it yet.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
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
