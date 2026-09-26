// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the inline error path of the four character friend calls on
// URedwoodClientInterface. The class exists apart from the subsystem so a
// test can build it without a world (RedwoodClientInterface.h header
// comment). A fresh instance has no realm socket, so every call must answer
// inline with "Not connected to Realm.", with or without a selected
// character. The realm guard comes first, as in every realm call of
// RedwoodClientInterface.cpp. The "No character selected." guard comes after
// it, so it needs a connected realm. The Redwood.Mock.* tests connect a realm,
// but only to a RedwoodBackend that runs in mock mode. This file keeps to
// tests that need no backend, so review covers that guard, not this test. The
// socket path is checked in PIE with a backend.

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

  // Logged in, in the realm, with a character.
  Client->bSentRealmConnected = true;
  Client->bAuthenticated = true;
  Client->PlayerId = TEXT("player-1");
  Client->SelectedCharacterId = TEXT("me-1");
  Client->Realm->bIsConnected = true;

  // The Realm socket drops and comes back; the re-handshake is in flight.
  Client->Realm->bIsConnected = false;
  Client->NoteRealmDrop();
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

  Client->ListCharacterFriends(OnList);
  Client->RequestCharacterFriend(TEXT("other-1"), OnError);
  Client->RespondToCharacterFriendRequest(TEXT("other-1"), true, OnError);
  Client->RemoveCharacterFriend(TEXT("other-1"), OnError);

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

  return true;
}
