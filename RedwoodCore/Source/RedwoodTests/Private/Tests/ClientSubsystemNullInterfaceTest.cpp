// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. It
// pins the null ClientInterface guards added to the social functions of
// URedwoodClientGameSubsystem for #2445.
//
// Initialize() builds ClientInterface only when ShouldUseBackend() is true, but
// each of these functions tests ShouldUseBackend() again at call time. In the
// editor that function reads bUseBackendInPIE, and a user can change it while
// PIE runs. A PIE session that starts with the backend off therefore has no
// interface, and it takes the backend branch as soon as the setting turns on.
// Before the guards, the branch read through a null pointer. That PIE toggle is
// the only way to reach it: a packaged client always builds the interface.
//
// This fixture does not use the toggle. It builds the same end state directly,
// with a subsystem that never ran Initialize() in a world of type Game, because
// that needs no editor setting and so gives the same result on every machine.
// The state it asserts is the one the guards answer, not a production path.
//
// Every guarded function is a separate call site with its own output type, so
// each is asserted. An unguarded site crashes the run instead of failing, so a
// pass here is also the evidence that no site was missed.
//
// The friend relay test below uses the same world, but it runs Initialize(),
// so the subsystem has a client interface and binds its relays.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "RedwoodClientGameSubsystem.h"
#include "RedwoodClientInterface.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Subsystems/SubsystemCollection.h"

#include "RedwoodFriendRelayProbe.h"

namespace {
  // Duplicated from the guards on purpose: the test pins the text the game and
  // the Blueprint async nodes actually show, rather than comparing the plugin's
  // own constant with itself.
  const TCHAR *const RedwoodExpectedGuardError =
    TEXT("Not connected to the backend.");

  // ShouldUseBackend() reads the world type, so a world of type Game makes the
  // backend path true without touching the editor's PIE backend setting. That
  // keeps the fixture independent of how the machine running the suite is set
  // up. The destructor releases the world, so nothing a test built is left on
  // the engine for the tests that run after it.
  struct FRedwoodClientSubsystemTestWorld {
    UWorld *World = nullptr;
    UGameInstance *GameInstance = nullptr;

    explicit FRedwoodClientSubsystemTestWorld(const TCHAR *WorldName) {
      World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
      if (!World) {
        return;
      }

      // A subsystem must live inside a UGameInstance, because
      // UGameInstanceSubsystem declares Within = GameInstance, and GetWorld()
      // resolves up that outer chain to the game instance's world context.
      // The context has to exist first for the game instance to adopt.
      FWorldContext &WorldContext =
        GEngine->CreateNewWorldContext(EWorldType::Game);
      WorldContext.SetCurrentWorld(World);

      GameInstance = NewObject<UGameInstance>(GEngine);
      WorldContext.OwningGameInstance = GameInstance;
      World->SetGameInstance(GameInstance);

      // Adopting the world this way instead of through InitializeStandalone()
      // skips UGameInstance::Init(), so the test does not build every other
      // game instance subsystem in the editor process.
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

  // Building the subsystem by hand keeps it out of the game instance's
  // subsystem collection, so Initialize() never runs and ClientInterface stays
  // null -- the exact state the guards exist for.
  URedwoodClientGameSubsystem *Subsystem =
    NewObject<URedwoodClientGameSubsystem>(TestWorld.GameInstance);

  // These two checks prove the fixture is the broken state. Without them the
  // calls below could pass for the wrong reason.
  TestNull(
    TEXT("Fixture has no client interface"), Subsystem->GetClientInterface()
  );
  TestTrue(
    TEXT("Fixture takes the backend path"),
    URedwoodCommonGameSubsystem::ShouldUseBackend(Subsystem->GetWorld())
  );

  FString Error;

  // Five output shapes cover all fourteen functions.
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

  // TestEqualSensitive, not TestEqual: the TCHAR* form of TestEqual compares
  // with Stricmp, so it would accept the text in the wrong case. This pins what
  // the player actually reads.
  //
  // Clearing after each check means a guard that returns without firing its
  // delegate fails here, instead of passing on the previous call's value.
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

  // The four character friend calls have the same guard.
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

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodClientSubsystemFriendRelayTest,
  "Redwood.ClientSubsystem.FriendRelay",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// The director listeners broadcast the friend delegates of the client
// interface. The game binds the events of the same name on the subsystem, so
// each interface broadcast must reach the subsystem event once.
bool FRedwoodClientSubsystemFriendRelayTest::RunTest(
  const FString &Parameters
) {
  FRedwoodClientSubsystemTestWorld TestWorld(TEXT("RedwoodFriendRelayWorld"));
  if (!TestNotNull(TEXT("Test world was created"), TestWorld.World)) {
    return false;
  }

  // Initialize() builds the client interface and binds the relays, as it does
  // in the game. It opens no socket.
  URedwoodClientGameSubsystem *Subsystem =
    NewObject<URedwoodClientGameSubsystem>(TestWorld.GameInstance);
  FSubsystemCollection<UGameInstanceSubsystem> Collection;
  Subsystem->Initialize(Collection);

  // Initialize() also listens for each new world, and that handler reads the
  // world of this test's game instance. The test world is gone after this
  // test, so the listener must go first.
  ON_SCOPE_EXIT {
    FWorldDelegates::OnPostWorldInitialization.RemoveAll(Subsystem);
    Subsystem->Deinitialize();
  };

  URedwoodClientInterface *ClientInterface = Subsystem->GetClientInterface();
  if (!TestNotNull(TEXT("Initialize built the interface"), ClientInterface)) {
    return false;
  }

  URedwoodFriendRelayProbe *Probe = NewObject<URedwoodFriendRelayProbe>();
  Subsystem->OnCharacterFriendAlert.AddDynamic(
    Probe, &URedwoodFriendRelayProbe::HandleCharacterFriendAlert
  );
  Subsystem->OnFriendRequestReceived.AddDynamic(
    Probe, &URedwoodFriendRelayProbe::HandleFriendRequestReceived
  );

  FRedwoodCharacterFriendAlert Alert;
  Alert.Type = ERedwoodCharacterFriendAlertType::Online;
  Alert.CharacterId = TEXT("me-1");
  Alert.OtherCharacterId = TEXT("other-1");
  Alert.OtherCharacterName = TEXT("Bob");
  Alert.ZoneName = TEXT("zone-1");
  ClientInterface->OnCharacterFriendAlert.Broadcast(Alert);

  TestEqual(
    TEXT("The character friend alert reached the subsystem once"),
    Probe->CharacterFriendAlertCount,
    1
  );
  const FRedwoodCharacterFriendAlert &Relayed =
    Probe->LastCharacterFriendAlert;
  TestTrue(TEXT("Same type"), Relayed.Type == Alert.Type);
  TestEqual(TEXT("Same character"), Relayed.CharacterId, Alert.CharacterId);
  TestEqual(
    TEXT("Same other character"),
    Relayed.OtherCharacterId,
    Alert.OtherCharacterId
  );
  TestEqual(
    TEXT("Same name"), Relayed.OtherCharacterName, Alert.OtherCharacterName
  );
  TestEqual(TEXT("Same zone"), Relayed.ZoneName, Alert.ZoneName);

  FRedwoodPlayer Requester;
  Requester.PlayerId = TEXT("player-1");
  ClientInterface->OnFriendRequestReceived.Broadcast(Requester);

  TestEqual(
    TEXT("The friend request reached the subsystem once"),
    Probe->FriendRequestCount,
    1
  );
  TestEqual(
    TEXT("Same requester"), Probe->LastRequesterId, Requester.PlayerId
  );

  return true;
}
