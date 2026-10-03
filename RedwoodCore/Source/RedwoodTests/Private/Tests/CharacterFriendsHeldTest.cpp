// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins that every character friend call goes through GateRealm and
// RealmReplies, with no backend. The Realm socket only looks connected: it is
// set by hand, as ReloginFailureTest.cpp does.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#include "RedwoodClientInterface.h"
#include "RedwoodPendingReplies.h"
#include "SocketIOClient.h"
#include "Types/RedwoodTypesCharacters.h"

// Sent before the Realm re-handshake, a call reaches a server socket that does
// not know the player and is refused, so the calls wait like other Realm
// requests (HollowedOath#2854). The socket never really connects, so a sent
// call is never answered.
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
    Client->CancelCharacterFriendRequest(TEXT("other-1"), OnError);
  };

  // No character is selected yet: the character guard answers each call.
  CallAll();
  TestEqual(TEXT("Every call answers at once"), Errors.Num(), 5);
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
    5
  );
  TestEqual(TEXT("No held call is answered yet"), Errors.Num(), 0);

  Client->EndRealmReauthentication(true);

  TestEqual(
    TEXT("Released: nothing stays held"), Client->RealmHeldRequests.Num(), 0
  );
  TestEqual(TEXT("Released: no call is refused"), Errors.Num(), 0);

  // HollowedOath#2886: a sent call waits for its reply, and a drop before the
  // reply fails it once.
  TestEqual(
    TEXT("Every sent call waits for its reply"), Client->RealmReplies.Num(), 5
  );
  Client->Realm->bIsConnected = false;
  Client->NoteRealmDrop();
  TestEqual(TEXT("The drop answers every sent call once"), Errors.Num(), 5);
  for (const FString &Error : Errors) {
    TestEqualSensitive(
      TEXT("A sent call tells its reply was lost"),
      *Error,
      FRedwoodPendingReplies::LostReplyError
    );
  }
  TestEqual(
    TEXT("No call waits after the drop"), Client->RealmReplies.Num(), 0
  );

  return true;
}
