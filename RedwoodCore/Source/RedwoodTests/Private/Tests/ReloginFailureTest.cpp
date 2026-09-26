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
//      ends, is held.
//   2. When the re-login fails, it fails with the not-connected error, and so
//      does a later request. A request that was emitted instead would get no
//      answer at all here, because no backend runs.
//   3. The same for a Realm request when the Realm re-handshake fails.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"

#include "ReconnectListener.h"
#include "RedwoodClientInterface.h"
#include "SocketIOClient.h"

namespace {
  // Logout writes an empty remember-me save game. Keep the developer's slot:
  // take it before the test and put it back after.
  struct FRedwoodSaveSlotGuard {
    const FString SlotName = TEXT("RedwoodSaveGame");
    TStrongObjectPtr<USaveGame> Saved;

    FRedwoodSaveSlotGuard() {
      if (UGameplayStatics::DoesSaveGameExist(SlotName, 0)) {
        Saved.Reset(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
      }
    }

    ~FRedwoodSaveSlotGuard() {
      if (Saved.IsValid()) {
        UGameplayStatics::SaveGameToSlot(Saved.Get(), SlotName, 0);
      } else {
        UGameplayStatics::DeleteGameInSlot(SlotName, 0);
      }
    }
  };

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
  TestEqual(
    TEXT("Director request is held"), Client->DirectorHeldRequests.Num(), 1
  );

  FErrorCapture Party;
  Client->InviteToParty(TEXT("player-2"), Party.MakeDelegate());
  TestEqual(TEXT("Realm request is held"), Client->RealmHeldRequests.Num(), 1);

  // The Director re-login fails: the reply empties the ids first.
  Client->PlayerId.Empty();
  Client->AuthToken.Empty();
  Client->EndDirectorReauthentication(false);

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

// A first connect that fails twice broadcasts a lost connection, and the game
// shows a message for it. When that connect then succeeds, a reestablished
// broadcast must follow, or the message stays up over a working socket. A
// later reconnect is left to the re-login, which broadcasts after it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodFirstConnectAfterLossTest,
  "Redwood.HeldRequests.FirstConnectAfterLoss",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodFirstConnectAfterLossTest::RunTest(const FString &Parameters) {
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();
  TStrongObjectPtr<URedwoodReconnectListener> Listener(
    NewObject<URedwoodReconnectListener>()
  );
  Listener->Watch(Client);

  // A first connect with no loss broadcast says nothing extra.
  Client->NoteFirstDirectorConnect();
  Client->NoteFirstRealmConnect();
  TestEqual(TEXT("Plain first Director connect"), Listener->DirectorCount, 0);
  TestEqual(TEXT("Plain first Realm connect"), Listener->RealmCount, 0);

  // Both first connects failed twice: the lost broadcasts went out, through
  // the same hooks the reconnection callbacks call.
  Client->NoteDirectorDrop();
  Client->NoteRealmDrop();
  Client->NoteFirstDirectorConnect();
  Client->NoteFirstRealmConnect();
  TestEqual(
    TEXT("First Director connect after a loss is reestablished"),
    Listener->DirectorCount,
    1
  );
  TestEqual(
    TEXT("First Realm connect after a loss is reestablished"),
    Listener->RealmCount,
    1
  );

  // A drop of an established socket waits for its re-login instead.
  Client->bSentDirectorConnected = true;
  Client->bSentRealmConnected = true;
  Client->NoteDirectorDrop();
  Client->NoteRealmDrop();
  Client->NoteFirstDirectorConnect();
  Client->NoteFirstRealmConnect();
  TestEqual(TEXT("Director reconnect waits"), Listener->DirectorCount, 1);
  TestEqual(TEXT("Realm reconnect waits"), Listener->RealmCount, 1);

  return true;
}

// Both sockets move. The Realm is back first and asks the Director for its
// re-handshake token; then the Director drops again before it answers. That
// answer is lost with the socket, so the re-handshake must be tried again,
// or bRealmReauthPending stays set and every Realm request fails for good.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodRealmReauthRetryTest,
  "Redwood.HeldRequests.RealmReauthSurvivesDirectorDrop",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodRealmReauthRetryTest::RunTest(const FString &Parameters) {
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  Client->Director = ISocketIOClientModule::Get().NewValidNativePointer();
  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Client->Director->bIsConnected = false;
    Client->Realm->bIsConnected = false;
    Client->Deinitialize();
  };

  Client->bSentDirectorConnected = true;
  Client->bSentRealmConnected = true;
  Client->bAuthenticated = true;
  Client->PlayerId = TEXT("player-1");
  Client->AuthToken = TEXT("token-1");
  Client->Director->bIsConnected = true;

  // The Realm is back and its re-handshake asks the Director. With the
  // Director logged in, it sends at once and waits for the answer, not on
  // the retry.
  Client->Realm->bIsConnected = true;
  Client->bRealmReauthPending = true;
  Client->BeginRealmReauthentication();
  TestFalse(
    TEXT("The re-handshake waits for its answer, not on the retry"),
    Client->TimerManager.IsTimerActive(Client->ReauthenticationAttemptTimer)
  );

  // The Director drops before it answers.
  Client->Director->bIsConnected = false;
  Client->NoteDirectorDrop();
  Client->bAuthenticated = false;

  TestTrue(
    TEXT("The lost re-handshake is tried again"),
    Client->TimerManager.IsTimerActive(Client->ReauthenticationAttemptTimer)
  );

  // The retry's answer ends the re-handshake, and no second one starts.
  Client->EndRealmReauthentication(true);
  TestFalse(
    TEXT("The finished re-handshake leaves no retry"),
    Client->TimerManager.IsTimerActive(Client->ReauthenticationAttemptTimer)
  );

  return true;
}

// A Logout in the silent grace after a Director drop, when the player is not
// authenticated, must end the session: the held requests fail, the ids go, and
// a re-login reply that lands after it does not log the player back in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodLogoutDuringGraceTest,
  "Redwood.HeldRequests.LogoutDuringGrace",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodLogoutDuringGraceTest::RunTest(const FString &Parameters) {
  FRedwoodSaveSlotGuard SaveSlot;
  TStrongObjectPtr<URedwoodClientInterface> Interface(
    NewObject<URedwoodClientInterface>()
  );
  URedwoodClientInterface *Client = Interface.Get();

  Client->Director = ISocketIOClientModule::Get().NewValidNativePointer();
  Client->Realm = ISocketIOClientModule::Get().NewValidNativePointer();
  ON_SCOPE_EXIT {
    Client->Director->bIsConnected = false;
    Client->Realm->bIsConnected = false;
    Client->Deinitialize();
  };

  Client->bSentDirectorConnected = true;
  Client->bSentRealmConnected = true;
  Client->bAuthenticated = true;
  Client->PlayerId = TEXT("player-1");
  Client->AuthToken = TEXT("token-1");

  // The Director drops and is back, but the player's re-login is not done,
  // so a Director request is held. The Realm stays down: Logout would close a
  // connected Realm socket, and none is open here.
  Client->NoteDirectorDrop();
  Client->bAuthenticated = false;
  Client->Director->bIsConnected = true;

  FErrorCapture Friend;
  Client->RequestFriend(TEXT("player-2"), Friend.MakeDelegate());
  TestEqual(
    TEXT("Director request is held"), Client->DirectorHeldRequests.Num(), 1
  );

  Client->Logout();

  TestTrue(TEXT("Logout clears the player id"), Client->PlayerId.IsEmpty());
  TestTrue(TEXT("Logout clears the token"), Client->AuthToken.IsEmpty());
  TestEqual(
    TEXT("Held request fails, it is not sent"),
    Friend.Error,
    TEXT("Not connected to Director.")
  );
  TestEqual(
    TEXT("Nothing stays held"), Client->DirectorHeldRequests.Num(), 0
  );

  // The re-login reply lands after the Logout, as the Login reply writes it.
  Client->PlayerId = TEXT("player-1");
  Client->AuthToken = TEXT("token-1");
  Client->bAuthenticated = true;
  Client->EndDirectorReauthentication(true);

  TestFalse(
    TEXT("A late re-login reply does not log the player back in"),
    Client->IsLoggedIn()
  );
  TestTrue(TEXT("Its token is dropped again"), Client->AuthToken.IsEmpty());

  return true;
}
