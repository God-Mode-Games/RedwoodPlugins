// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the guards of the four character friend calls on
// URedwoodClientInterface, with no backend. The class exists apart from the
// subsystem so a test can build it without a world (RedwoodClientInterface.h
// header comment). A fresh instance has no realm socket, so every call must
// answer inline with "Not connected to Realm.", with or without a selected
// character: the realm guard comes first, as in every realm call of
// RedwoodClientInterface.cpp. The guards after it need a realm socket that
// looks connected. HeldWhileRealmReconnects makes one by hand, as
// ReloginFailureTest.cpp does, and pins "No character selected." and the hold
// while the realm socket reconnects. The answer of a real realm is checked in
// PIE with a backend.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"
#include "Types/RedwoodTypesCharacters.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsNotConnectedTest,
  "Redwood.CharacterFriends.NotConnected",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsNotConnectedTest::RunTest(
  const FString &Parameters
) {
  URedwoodClientInterface *Redwood = NewObject<URedwoodClientInterface>();

  FString Error;
  FRedwoodListCharacterFriendsOutputDelegate OnList =
    FRedwoodListCharacterFriendsOutputDelegate::CreateLambda(
      [&Error](const FRedwoodListCharacterFriendsOutput &Output) {
        Error = Output.Error;
      }
    );
  FRedwoodErrorOutputDelegate OnError =
    FRedwoodErrorOutputDelegate::CreateLambda(
      [&Error](const FString &Output) { Error = Output; }
    );

  // TestEqualSensitive: the TCHAR* form of TestEqual compares without case.
  // Clearing after each check makes a call that never answers fail here.
  const auto CheckAll = [&](const TCHAR *Expected, const TCHAR *Case) {
    Redwood->ListCharacterFriends(OnList);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RequestCharacterFriend(TEXT("other-1"), OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RespondToCharacterFriendRequest(TEXT("other-1"), true, OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RemoveCharacterFriend(TEXT("other-1"), OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();
  };

  // With no realm and no character, the realm error wins: the realm guard is
  // first.
  CheckAll(TEXT("Not connected to Realm."), TEXT("No realm, no character"));

  // SetSelectedCharacter keeps the id and sends nothing without a realm.
  Redwood->SetSelectedCharacter(TEXT("me-1"));
  CheckAll(TEXT("Not connected to Realm."), TEXT("No realm, a character"));

  return true;
}

// HollowedOath#2854 made a Realm request that the player makes while the Realm
// socket reconnects wait for the re-handshake (GateRealm). The character
// friend calls must wait too. Sent before the re-handshake, they reach a
// server socket that does not know the player: the game's friends list
// fetch at the Director reconnect then fails, and a command gets refused.
// The sockets never connect: bIsConnected is set by hand, as in
// ReloginFailureTest.cpp, so a call that is emitted is never answered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsHeldTest,
  "Redwood.CharacterFriends.HeldWhileRealmReconnects",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsHeldTest::RunTest(const FString &Parameters) {
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Client->Realm->bIsConnected = false;
    Client->Deinitialize();
  };

  // Logged in and in the realm.
  Client->bSentRealmConnected = true;
  Client->bAuthenticated = true;
  Client->PlayerId = TEXT("player-1");
  Client->Realm->bIsConnected = true;

  TArray<FString> Errors;
  FRedwoodListCharacterFriendsOutputDelegate OnList =
    FRedwoodListCharacterFriendsOutputDelegate::CreateLambda(
      [&Errors](const FRedwoodListCharacterFriendsOutput &Output) {
        Errors.Add(Output.Error);
      }
    );
  FRedwoodErrorOutputDelegate OnError =
    FRedwoodErrorOutputDelegate::CreateLambda(
      [&Errors](const FString &Output) { Errors.Add(Output); }
    );
  const auto CallAll = [&]() {
    Client->ListCharacterFriends(OnList);
    Client->RequestCharacterFriend(TEXT("other-1"), OnError);
    Client->RespondToCharacterFriendRequest(TEXT("other-1"), true, OnError);
    Client->RemoveCharacterFriend(TEXT("other-1"), OnError);
  };

  // No character is selected yet: the realm check passes, and the character
  // guard answers each call inline.
  CallAll();
  TestEqual(TEXT("Every call answers at once"), Errors.Num(), 4);
  for (const FString &Error : Errors) {
    TestEqualSensitive(
      TEXT("The character guard answers"), *Error, TEXT("No character selected.")
    );
  }
  Errors.Reset();
  Client->SelectedCharacterId = TEXT("me-1");

  // The Realm socket drops and comes back; the re-handshake is in flight.
  Client->Realm->bIsConnected = false;
  Client->NoteRealmDrop();
  Client->Realm->bIsConnected = true;

  CallAll();

  TestEqual(
    TEXT("Every character friend call is held"),
    Client->RealmHeldRequests.Num(),
    4
  );
  TestEqual(TEXT("No held call is answered yet"), Errors.Num(), 0);

  // The re-handshake fails: every held call answers with the realm error.
  Client->EndRealmReauthentication(false);

  TestEqual(TEXT("Every held call is answered"), Errors.Num(), 4);
  for (const FString &Error : Errors) {
    TestEqualSensitive(
      TEXT("A held call fails, it is not sent"),
      *Error,
      TEXT("Not connected to Realm.")
    );
  }
  Errors.Reset();

  // A second drop, and this time the re-handshake succeeds: each held call
  // is sent once. No backend runs, so a sent call gets no answer; a call that
  // answers was refused, and a call still held was not sent.
  Client->Realm->bIsConnected = false;
  Client->NoteRealmDrop();
  Client->Realm->bIsConnected = true;
  CallAll();
  TestEqual(
    TEXT("Held again after the second drop"), Client->RealmHeldRequests.Num(), 4
  );

  Client->EndRealmReauthentication(true);

  TestEqual(
    TEXT("Released: nothing stays held"), Client->RealmHeldRequests.Num(), 0
  );
  TestEqual(TEXT("Released: no call is refused"), Errors.Num(), 0);

  // The end of a grace after the release runs no call a second time.
  Client->RealmHeldRequests.Expire(Client->TimerManager);
  TestEqual(TEXT("Released: no call runs again"), Errors.Num(), 0);

  return true;
}
