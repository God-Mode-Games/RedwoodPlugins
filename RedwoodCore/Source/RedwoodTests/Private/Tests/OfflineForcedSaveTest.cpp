// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins that a forced OFFLINE character save (the logout flush) serializes every enabled group,
// not just the dirty ones. SaveCharacterJsonToDisk replaces the whole file, so a forced save that
// sent only dirty groups wiped the clean ones (class, appearance, wallet) from the character.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"

// Named, not anonymous: test files share a translation unit under the unity build.
namespace RedwoodOfflineForcedSaveTest {
  const FString TestCharacterId = TEXT("redwood-test-offline-forced-save");

  // A PIE world takes the offline path while bUseBackendInPIE is off (the default). The
  // destructor releases the world so later tests do not see it.
  struct FOfflineWorld {
    UWorld *World = nullptr;
    UGameInstance *GameInstance = nullptr;

    FOfflineWorld() {
      World = UWorld::CreateWorld(
        EWorldType::PIE, false, TEXT("RedwoodOfflineForcedSaveWorld")
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodOfflineForcedSaveKeepsCleanGroupsTest,
  "Redwood.Persistence.OfflineForcedSaveKeepsCleanGroups",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodOfflineForcedSaveKeepsCleanGroupsTest::RunTest(
  const FString &Parameters
) {
  namespace Fixture = RedwoodOfflineForcedSaveTest;
  Fixture::FOfflineWorld TestWorld;
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }
  URedwoodServerGameSubsystem *Subsystem =
    NewObject<URedwoodServerGameSubsystem>(TestWorld.GameInstance);
  if (!TestFalse(
        TEXT("Fixture takes the offline path"),
        URedwoodCommonGameSubsystem::ShouldUseBackend(TestWorld.World)
      )) {
    return false;
  }

  APlayerState *PlayerState = TestWorld.World->SpawnActor<APlayerState>();
  if (!TestNotNull(TEXT("Player state spawned"), PlayerState)) {
    return false;
  }
  URedwoodPlayerStateComponent *StateComponent =
    NewObject<URedwoodPlayerStateComponent>(PlayerState);
  StateComponent->RegisterComponent();
  StateComponent->RedwoodCharacter.Id = Fixture::TestCharacterId;

  URedwoodCharacterComponent *CharacterComponent =
    NewObject<URedwoodCharacterComponent>(PlayerState);
  CharacterComponent->RegisterComponent();
  CharacterComponent->RedwoodCharacterId = Fixture::TestCharacterId;
  // Only the creator data group is enabled, and it reads a struct every actor already has, so
  // the test needs no game class. Every other group would log an error for its missing variable.
  CharacterComponent->bUseCharacterCreatorData = true;
  CharacterComponent->CharacterCreatorDataVariableName =
    TEXT("ReplicatedMovement");
  CharacterComponent->bUseMetadata = false;
  CharacterComponent->bUseEquippedInventory = false;
  CharacterComponent->bUseNonequippedInventory = false;
  CharacterComponent->bUseItems = false;
  CharacterComponent->bUseProgress = false;
  CharacterComponent->bUseData = false;
  CharacterComponent->bUseAbilitySystem = false;
  CharacterComponent->ClearDirtyFlags();

  // Control: the group is clean, so the old condition would skip it.
  TestFalse(
    TEXT("Creator data is clean"),
    CharacterComponent->IsCharacterCreatorDataDirty()
  );

  TSharedPtr<FJsonObject> Saved =
    Subsystem->CreatePlayerCharacterDataObject(PlayerState, /*bForce*/ true);
  if (!TestTrue(TEXT("A forced offline save produces a document"), Saved.IsValid())) {
    return false;
  }
  TestTrue(
    TEXT("The clean creator data group is still written"),
    Saved->HasTypedField<EJson::Object>(TEXT("characterCreatorData"))
  );
  return true;
}
