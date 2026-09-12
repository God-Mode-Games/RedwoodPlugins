// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. It
// pins the null ClientInterface guards added to the social functions of
// URedwoodClientGameSubsystem for #2445.
//
// ClientInterface is only built if Initialize() ran while the backend was on,
// but each of these functions gates on ShouldUseBackend(), which is read again
// at every call. The two states therefore disagree: a subsystem that never
// initialized the interface, or a PIE session where the backend setting turned
// on after Initialize(), reaches the backend branch with a null interface.
// Before the guards that branch dereferenced null. GetPlayerId() has always
// checked, which is why the omission is a defect and not a design.
//
// Every guarded function is a separate call site with its own output type, so
// each is asserted. An unguarded site crashes the run instead of failing, so a
// pass here is also the evidence that no site was missed.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "RedwoodClientGameSubsystem.h"
#include "RedwoodCommonGameSubsystem.h"

namespace {
  // Duplicated from the guards on purpose: the test pins the text the game and
  // the Blueprint async nodes actually show, rather than comparing the plugin's
  // own constant with itself.
  const TCHAR *const RedwoodExpectedGuardError =
    TEXT("Not connected to the backend.");
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodClientSubsystemNullInterfaceGuardsTest,
  "Redwood.ClientSubsystem.NullInterfaceGuards",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodClientSubsystemNullInterfaceGuardsTest::RunTest(
  const FString &Parameters
) {
  // ShouldUseBackend() reads the world type, so a world of type Game makes the
  // backend path true without touching the editor's PIE backend setting. That
  // keeps the fixture independent of how the machine running the suite is set
  // up.
  UWorld *World = UWorld::CreateWorld(
    EWorldType::Game, false, TEXT("RedwoodNullInterfaceGuardWorld")
  );
  if (!TestNotNull(TEXT("Test world was created"), World)) {
    return false;
  }

  // The subsystem must live inside a UGameInstance, because
  // UGameInstanceSubsystem declares Within = GameInstance, and GetWorld()
  // resolves up that outer chain to the game instance's world context. The
  // context has to exist first for the game instance to adopt.
  FWorldContext &WorldContext =
    GEngine->CreateNewWorldContext(EWorldType::Game);
  WorldContext.SetCurrentWorld(World);

  UGameInstance *GameInstance = NewObject<UGameInstance>(GEngine);
  WorldContext.OwningGameInstance = GameInstance;
  World->SetGameInstance(GameInstance);

  // Adopting the world this way instead of through InitializeStandalone()
  // skips UGameInstance::Init(), so the test does not build every other game
  // instance subsystem in the editor process.
  GameInstance->OnWorldChanged(nullptr, World);

  // Building the subsystem by hand keeps it out of the game instance's
  // subsystem collection, so Initialize() never runs and ClientInterface stays
  // null -- the exact state the guards exist for.
  URedwoodClientGameSubsystem *Subsystem =
    NewObject<URedwoodClientGameSubsystem>(GameInstance);

  // Nothing below means anything unless the fixture really is the broken state,
  // so both halves of it are asserted first.
  TestNull(
    TEXT("Fixture has no client interface"), Subsystem->GetClientInterface()
  );
  TestTrue(
    TEXT("Fixture takes the backend path"),
    URedwoodCommonGameSubsystem::ShouldUseBackend(Subsystem->GetWorld())
  );

  FString Error;

  // Four output shapes cover all ten functions.
  FRedwoodListPlayersOutputDelegate OnListPlayers =
    FRedwoodListPlayersOutputDelegate::CreateLambda(
      [&Error](const FRedwoodListPlayersOutput &Output) {
        Error = Output.Error;
      }
    );
  FRedwoodPlayerOutputDelegate OnPlayer =
    FRedwoodPlayerOutputDelegate::CreateLambda(
      [&Error](const FRedwoodPlayerOutput &Output) { Error = Output.Error; }
    );
  FRedwoodListRealmContactsOutputDelegate OnListRealmContacts =
    FRedwoodListRealmContactsOutputDelegate::CreateLambda(
      [&Error](const FRedwoodListRealmContactsOutput &Output) {
        Error = Output.Error;
      }
    );
  FRedwoodErrorOutputDelegate OnError =
    FRedwoodErrorOutputDelegate::CreateLambda(
      [&Error](const FString &Output) { Error = Output; }
    );

  // Clearing after each check means a guard that returns without firing its
  // delegate fails here, instead of passing on the previous call's value.
  const auto CheckGuard = [this, &Error](const TCHAR *What) {
    TestEqual(What, *Error, RedwoodExpectedGuardError);
    Error.Reset();
  };

  Subsystem->SearchForPlayers(TEXT("someone"), false, OnListPlayers);
  CheckGuard(TEXT("SearchForPlayers reports the error"));

  Subsystem->SearchForPlayerById(TEXT("player-1"), OnPlayer);
  CheckGuard(TEXT("SearchForPlayerById reports the error"));

  Subsystem->ListFriends(ERedwoodFriendListType::Active, OnListPlayers);
  CheckGuard(TEXT("ListFriends reports the error"));

  Subsystem->RequestFriend(TEXT("player-1"), OnError);
  CheckGuard(TEXT("RequestFriend reports the error"));

  Subsystem->RemoveFriend(TEXT("player-1"), OnError);
  CheckGuard(TEXT("RemoveFriend reports the error"));

  Subsystem->RespondToFriendRequest(TEXT("player-1"), true, OnError);
  CheckGuard(TEXT("RespondToFriendRequest reports the error"));

  Subsystem->SetPlayerBlocked(TEXT("player-1"), true, OnError);
  CheckGuard(TEXT("SetPlayerBlocked reports the error"));

  Subsystem->ListRealmContacts(OnListRealmContacts);
  CheckGuard(TEXT("ListRealmContacts reports the error"));

  Subsystem->AddRealmContact(TEXT("character-1"), false, OnError);
  CheckGuard(TEXT("AddRealmContact reports the error"));

  Subsystem->RemoveRealmContact(TEXT("character-1"), OnError);
  CheckGuard(TEXT("RemoveRealmContact reports the error"));

  // Release the world before returning, so nothing this test built is left on
  // the engine for the tests that run after it.
  GameInstance->OnWorldChanged(World, nullptr);
  GEngine->DestroyWorldContext(World);
  World->DestroyWorld(false);

  return true;
}
