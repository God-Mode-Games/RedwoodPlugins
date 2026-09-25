// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the wiring of the held requests in URedwoodClientInterface for a failed
// re-login after a backend move (HollowedOath#2854). The Login reply writes
// PlayerId and AuthToken before it checks the error, so a failed re-login
// leaves both EMPTY. The held requests must still fail through their error
// path then. If they are sent instead, they reach a socket that does not know
// the player, and several routes answer that with an empty error: a false
// success.
//   1. A Director request made after the socket is back, before the re-login
//      ends, is held (no answer yet).
//   2. When the re-login fails, it fails with the not-connected error, and so
//      does a later request. A request that was emitted instead would get no
//      answer at all here, because no backend runs.
//   3. The same for a Realm request when the Realm re-handshake fails.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"

namespace {
  struct FErrorCapture {
    bool bAnswered = false;
    FString Error;

    FRedwoodErrorOutputDelegate MakeDelegate() {
      return FRedwoodErrorOutputDelegate::CreateLambda(
        [this](const FString &InError) {
          bAnswered = true;
          Error = InError;
        }
      );
    }
  };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodReloginFailureTest,
  "Redwood.HeldRequests.ReloginFailure",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodReloginFailureTest::RunTest(const FString &Parameters) {
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  // Never connected: bIsConnected is set by hand, so nothing reaches a
  // network, and an emitted request is never answered.
  Client->Director = ISocketIOClientModule::Get().NewValidNativePointer();
  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Client->Director->bIsConnected = false;
    Client->Realm->bIsConnected = false;
    Client->Deinitialize();
  };

  // Logged in and in the realm.
  Client->bSentDirectorConnected = true;
  Client->bSentRealmConnected = true;
  Client->bAuthenticated = true;
  Client->PlayerId = TEXT("player-1");
  Client->AuthToken = TEXT("token-1");
  Client->Director->bIsConnected = true;
  Client->Realm->bIsConnected = true;

  // Both sockets drop, as the reconnection listeners do it.
  Client->Director->bIsConnected = false;
  Client->NoteDirectorDrop();
  Client->bAuthenticated = false;
  Client->Realm->bIsConnected = false;
  Client->bRealmReauthPending = true;

  // Both sockets are back; the re-logins are in flight.
  Client->Director->bIsConnected = true;
  Client->Realm->bIsConnected = true;

  FErrorCapture Friend;
  Client->RequestFriend(TEXT("player-2"), Friend.MakeDelegate());
  TestFalse(TEXT("Director request is held"), Friend.bAnswered);

  FErrorCapture Party;
  Client->InviteToParty(TEXT("player-2"), Party.MakeDelegate());
  TestFalse(TEXT("Realm request is held"), Party.bAnswered);

  // The Director re-login fails: the reply empties the ids first.
  Client->PlayerId.Empty();
  Client->AuthToken.Empty();
  Client->DirectorHeldRequests.Expire(Client->TimerManager);

  TestTrue(TEXT("Held Director request is answered"), Friend.bAnswered);
  TestEqual(
    TEXT("Held Director request fails, it is not sent"),
    Friend.Error,
    TEXT("Not connected to Director.")
  );

  FErrorCapture LaterFriend;
  Client->RequestFriend(TEXT("player-3"), LaterFriend.MakeDelegate());
  TestEqual(
    TEXT("Later Director request fails at once"),
    LaterFriend.Error,
    TEXT("Not connected to Director.")
  );

  // The Realm re-handshake fails too.
  Client->EndRealmReauthentication(false);

  TestTrue(TEXT("Held Realm request is answered"), Party.bAnswered);
  TestEqual(
    TEXT("Held Realm request fails, it is not sent"),
    Party.Error,
    TEXT("Not connected to Realm.")
  );

  FErrorCapture LaterParty;
  Client->InviteToParty(TEXT("player-3"), LaterParty.MakeDelegate());
  TestEqual(
    TEXT("Later Realm request fails at once"),
    LaterParty.Error,
    TEXT("Not connected to Realm.")
  );

  return true;
}
