// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the null ClientInterface guards of #2445. The fixture builds that state
// directly (no Initialize() in a Game world), not through the PIE toggle, so
// it does not depend on editor settings. An unguarded call crashes the run.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "RedwoodClientGameSubsystem.h"
#include "RedwoodClientInterface.h"
#include "RedwoodCommonGameSubsystem.h"

namespace {
  // Not the plugin constant: this pins the text the player sees.
  const TCHAR *const RedwoodExpectedGuardError =
    TEXT("Not connected to the backend.");

  // A Game world makes ShouldUseBackend() true with no editor setting. The
  // destructor releases the world, so later tests do not see it.
  struct FRedwoodClientSubsystemTestWorld {
    UWorld *World = nullptr;
    UGameInstance *GameInstance = nullptr;

    explicit FRedwoodClientSubsystemTestWorld(const TCHAR *WorldName) {
      World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
      if (!World) {
        return;
      }

      // The subsystem finds its world through the game instance's world
      // context, so the context comes first.
      FWorldContext &WorldContext =
        GEngine->CreateNewWorldContext(EWorldType::Game);
      WorldContext.SetCurrentWorld(World);

      GameInstance = NewObject<UGameInstance>(GEngine);
      WorldContext.OwningGameInstance = GameInstance;
      World->SetGameInstance(GameInstance);

      // Not InitializeStandalone(): Init() would build every other game
      // instance subsystem.
      GameInstance->OnWorldChanged(nullptr, World);
    }

    ~FRedwoodClientSubsystemTestWorld() {
      if (!World) {
        return;
      }
      GameInstance->OnWorldChanged(World, nullptr);
      GEngine->DestroyWorldContext(World);
      World->DestroyWorld(false);
    }
  };
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodClientSubsystemNullInterfaceGuardsTest,
  "Redwood.ClientSubsystem.NullInterfaceGuards",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodClientSubsystemNullInterfaceGuardsTest::RunTest(
  const FString &Parameters
) {
  FRedwoodClientSubsystemTestWorld TestWorld(
    TEXT("RedwoodNullInterfaceGuardWorld")
  );
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }

  // Built by hand, so Initialize() never runs and ClientInterface stays null.
  URedwoodClientGameSubsystem *Subsystem =
    NewObject<URedwoodClientGameSubsystem>(TestWorld.GameInstance);

  // Without these, the calls below could pass for the wrong reason.
  TestNull(
    TEXT("Fixture has no client interface"), Subsystem->GetClientInterface()
  );
  TestTrue(
    TEXT("Fixture takes the backend path"),
    URedwoodCommonGameSubsystem::ShouldUseBackend(Subsystem->GetWorld())
  );

  FString Error;

  // Five output shapes cover all fifteen functions.
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
  FRedwoodListCharacterFriendsOutputDelegate OnListCharacterFriends =
    FRedwoodListCharacterFriendsOutputDelegate::CreateLambda(
      [&Error](const FRedwoodListCharacterFriendsOutput &Output) {
        Error = Output.Error;
      }
    );

  // TestEqualSensitive: the TCHAR* form of TestEqual ignores case. The reset
  // makes a guard that never answers fail.
  const auto CheckGuard = [this, &Error](const TCHAR *What) {
    TestEqualSensitive(What, *Error, RedwoodExpectedGuardError);
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

  // The character friend calls have the same guard.
  Subsystem->ListCharacterFriends(OnListCharacterFriends);
  CheckGuard(TEXT("ListCharacterFriends reports the error"));

  Subsystem->RequestCharacterFriend(TEXT("character-1"), OnError);
  CheckGuard(TEXT("RequestCharacterFriend reports the error"));

  Subsystem->RespondToCharacterFriendRequest(
    TEXT("character-1"), true, OnError
  );
  CheckGuard(TEXT("RespondToCharacterFriendRequest reports the error"));

  Subsystem->RemoveCharacterFriend(TEXT("character-1"), OnError);
  CheckGuard(TEXT("RemoveCharacterFriend reports the error"));

  Subsystem->CancelCharacterFriendRequest(TEXT("character-1"), OnError);
  CheckGuard(TEXT("CancelCharacterFriendRequest reports the error"));

  return true;
}
