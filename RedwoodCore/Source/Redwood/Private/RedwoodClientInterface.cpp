// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodClientInterface.h"
#include "UObject/StrongObjectPtr.h" // FORK(hollowed-oath)
#include "RedwoodClientGameSubsystem.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodGameplayTags.h"
#include "RedwoodSaveGame.h"
#include "RedwoodSettings.h"

#include "GameFramework/GameplayMessageSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetStringLibrary.h"
#include "LatencyCheckerLibrary.h"
#include "Misc/DateTime.h"
#include "SIOJConvert.h"
#include "TimerManager.h"

void URedwoodClientInterface::Deinitialize() {
  // FORK(hollowed-oath): HollowedOath#2886. The callers go away too, so a
  // request that waits for a reply is dropped, not failed.
  Replies.Reset();

  if (Connection.IsValid()) {
    Connection->Close();
    Connection.Reset();
  }

  // FORK(hollowed-oath): HollowedOath#2854. The callers are going away too, so
  // a held request is dropped, not failed.
  HeldRequests.Reset(TimerManager);
}

// FORK(hollowed-oath): HollowedOath#2854. A backend move drops a socket for a
// few seconds. A request made then used to fail at once ("Not connected"), or,
// once the socket was back but before the re-login, reach a server socket that
// did not know the player and be refused. The gate holds it and sends it after
// the re-login instead; FRedwoodHeldRequests bounds the wait. True means the
// request was held or failed, and the caller returns. When it lets a request
// through, the socket is connected, so the upstream check after it passes.
// Not gated: login and the handshakes (they are the re-login), ticketing and
// proxies (a late join can move a player who has given up waiting),
// CreateCharacter (the creation screen shows its failure), and requests with
// no reply (a late emote is worse than none; the Director re-login re-sends
// the online character). A held request that carries a USIOJsonObject keeps
// it alive with a TStrongObjectPtr.
namespace {
  void SetRedwoodGateError(FString &Output, const TCHAR *Error) {
    Output = Error;
  }

  template <typename TOutput>
  void SetRedwoodGateError(TOutput &Output, const TCHAR *Error) {
    Output.Error = Error;
  }

  // FORK(hollowed-oath): HollowedOath#2886. Register and the OAuth logins
  // answer with an auth update, which has no Error field.
  void SetRedwoodGateError(FRedwoodAuthUpdate &Output, const TCHAR *Error) {
    Output.Type = ERedwoodAuthUpdateType::Error;
    Output.Message = Error;
  }

  // FORK(hollowed-oath): what the player reads for a join that Bifrost
  // refused with a reason and no message of its own (protocol.md, "Join").
  FString JoinRefusalText(const FString &Reason) {
    if (Reason == TEXT("rate_limited")) {
      return TEXT("Too many login attempts. Try again in a minute.");
    }
    if (Reason == TEXT("unavailable")) {
      return TEXT("The login service did not answer. Try again.");
    }
    if (Reason == TEXT("replaced")) {
      return TEXT("A newer login of this account finished first.");
    }
    if (Reason == TEXT("invalid_request")) {
      return TEXT("The client and the server do not speak the same protocol.");
    }
    return Reason;
  }

  // FORK(hollowed-oath): HollowedOath#2886. What a request whose reply was
  // lost tells its caller.
  template <typename TOutput>
  TFunction<void()> MakeLostReply(
    const TDelegate<void(const TOutput &)> &OnOutput
  ) {
    return [OnOutput]() {
      TOutput Output;
      SetRedwoodGateError(Output, FRedwoodPendingReplies::LostReplyError);
      OnOutput.ExecuteIfBound(Output);
    };
  }
}

template <typename TOutput>
bool URedwoodClientInterface::Gate(
  bool bSessionEstablished,
  bool bCanSend,
  const TCHAR *NotConnectedError,
  TFunction<void()> Request,
  const TDelegate<void(const TOutput &)> &OnOutput
) {
  if (HeldRequests.HoldIfReconnecting(
        bSessionEstablished, bCanSend, MoveTemp(Request), TimerManager
      )) {
    return true;
  }

  if (bCanSend) {
    return false;
  }

  TOutput Output;
  SetRedwoodGateError(Output, NotConnectedError);
  OnOutput.ExecuteIfBound(Output);
  return true;
}

template <typename TOutput>
bool URedwoodClientInterface::GateDirector(
  TFunction<void()> Request, const TDelegate<void(const TOutput &)> &OnOutput
) {
  return Gate(
    Connection.IsValid() && bSentConnected && HasPlayerSession(),
    CanSendToDirector(),
    TEXT("Not connected to Director."),
    MoveTemp(Request),
    OnOutput
  );
}

template <typename TOutput>
bool URedwoodClientInterface::GateRealm(
  TFunction<void()> Request, const TDelegate<void(const TOutput &)> &OnOutput
) {
  // FORK(hollowed-oath): CurrentRealmId is set when the session entered a
  // realm and cleared when it ends, so a realm request made before the
  // entry, or after a logout, fails at once instead of waiting a grace.
  return Gate(
    Connection.IsValid() && bSentConnected && HasPlayerSession() &&
      !CurrentRealmId.IsEmpty(),
    CanSendToRealm(),
    TEXT("Not connected to Realm."),
    MoveTemp(Request),
    OnOutput
  );
}

// FORK(hollowed-oath) BEGIN: HollowedOath#2886. The reply callback of a
// request; a drop before the reply fails it through OnOutput, once. Every
// request with a reply goes through it or through Replies.Track, except the
// logins and the Realm handshake: the re-login retries a lost reply, and a
// failure there would end the session.
template <typename TOutput>
FRedwoodReplyCallback URedwoodClientInterface::TrackReply(
  FRedwoodReplyCallback OnReply,
  const TDelegate<void(const TOutput &)> &OnOutput
) {
  return Replies.Track(MoveTemp(OnReply), MakeLostReply(OnOutput));
}

// The travel of an assignment finished, even after the game gave up on it
// (its enter-world timer can fire during the travel). The player is in the
// world: later zone transfers must move them, and the ticket is used up.
// Only the travel of an accepted assignment counts: any other client world
// (a PIE client, a future lobby) must not open the guard for a replay.
void URedwoodClientInterface::NoteArrivedInWorld() {
  if (!bTravelPending) {
    return;
  }
  bTravelPending = false;
  bInWorld = true;
  // A join sent after this travel's one owns the flags now; its lost reply
  // must keep its dropped assignment and its owed leave. When no leave is
  // owed, that join is still out or was left, and the player, who is in the
  // world now, must get zone transfers.
  if (TravelJoinSequence != JoinSequence) {
    if (!bLeaveTicketingOwed) {
      bAbandonedQueueJoin = false;
      bAssignmentExpected = true;
    }
    return;
  }
  bAbandonedQueueJoin = false;
  bAssignmentExpected = true;
  bLeaveTicketingOwed = false;
}

// Back at the entry level without a Logout or a new character: the player is
// at character select, where no assignment may move them.
void URedwoodClientInterface::NoteLeftWorld() {
  bAssignmentExpected = false;
  bInWorld = false;
  bTravelPending = false;
}

// A join replaces any ticket the server kept, so a leave owed for it would
// cancel the new one. It also replaces a dropped join, from any entry point
// (a ticketing join, a create that joins, a proxy join).
void URedwoodClientInterface::NoteJoinSent() {
  ++JoinSequence;
  bAssignmentExpected = true;
  bAbandonedQueueJoin = false;
  bLeaveTicketingOwed = false;
  // This client's own join: its assignment names a ticket it was not told
  // of, so the party ticket's check must not refuse it.
  AcceptedPartyTicketId.Reset();
}

namespace {
  // Keeps the last few ids. Answers false when Id is among them already.
  bool RememberRecentId(TArray<FString> &Ids, const FString &Id) {
    // A notice is resent within seconds; a few dozen covers any burst.
    constexpr int32 MaxRememberedIds = 32;
    if (Ids.Contains(Id)) {
      return false;
    }
    if (Ids.Num() >= MaxRememberedIds) {
      Ids.RemoveAt(0);
    }
    Ids.Add(Id);
    return true;
  }

  // A Redis resend repeats the messageId; a notice with none is never a repeat.
  bool IsRepeatedNotice(TArray<FString> &RecentIds, const FJsonObject &Message) {
    FString MessageId;
    Message.TryGetStringField(TEXT("messageId"), MessageId);
    return !MessageId.IsEmpty() && !RememberRecentId(RecentIds, MessageId);
  }
}

// The same notice can come again: a Redis resend (same messageId), or a
// worker pass or rejoin replay (a new messageId, the same ticketId).
void URedwoodClientInterface::HandlePartyQueued(
  const TSharedPtr<FJsonObject> &Message
) {
  if (!Message.IsValid()) {
    return;
  }
  FString CharacterId;
  FString TicketId;
  Message->TryGetStringField(TEXT("characterId"), CharacterId);
  Message->TryGetStringField(TEXT("ticketId"), TicketId);
  if (IsRepeatedNotice(RecentPartyTicketMessageIds, *Message) ||
      TicketId.IsEmpty() || CharacterId.IsEmpty()) {
    return;
  }
  if (SelectedCharacterId.IsEmpty()) {
    PendingPartyTicketId = TicketId;
    PendingPartyCharacterId = CharacterId;
    return;
  }
  if (CharacterId == SelectedCharacterId) {
    AcceptPartyTicket(TicketId);
  }
}

// A repeat of the accepted ticket must not undo a leave the game made since
// (bAbandonedQueueJoin). A ticket that ended gets no assignment.
void URedwoodClientInterface::AcceptPartyTicket(const FString &TicketId) {
  if (TicketId == AcceptedPartyTicketId ||
      EndedPartyTicketIds.Contains(TicketId)) {
    return;
  }
  // This member's own join, still out, keeps its own ticket: leave it, or
  // its solo assignment could move the player after the party's travel.
  if (bAssignmentExpected && AcceptedPartyTicketId.IsEmpty() && !bInWorld &&
      !bTravelPending) {
    bLeaveTicketingOwed = true;
  }
  // A leader has one ticket, so a stale "queued" of the replaced one
  // (HollowedOath#3031) must not take it back.
  if (!AcceptedPartyTicketId.IsEmpty()) {
    EndPartyTicket(AcceptedPartyTicketId);
  }
  // As NoteJoinSent, but an owed leave stays and goes now: it cannot end
  // the leader's ticket, and the party's travel would erase it.
  ++JoinSequence;
  bAssignmentExpected = true;
  bAbandonedQueueJoin = false;
  AcceptedPartyTicketId = TicketId;
  if (IsRealmReady()) {
    SendOwedLeave();
  }
}

bool URedwoodClientInterface::EndPartyTicket(const FString &TicketId) {
  RememberRecentId(EndedPartyTicketIds, TicketId);
  if (TicketId != AcceptedPartyTicketId) {
    return false;
  }
  AcceptedPartyTicketId.Reset();
  return true;
}

// Only the named ticket: a late "left" of an older ticket must not drop the
// join of a newer one.
void URedwoodClientInterface::HandlePartyLeft(
  const TSharedPtr<FJsonObject> &Message
) {
  if (!Message.IsValid()) {
    return;
  }
  FString TicketId;
  Message->TryGetStringField(TEXT("ticketId"), TicketId);
  if (IsRepeatedNotice(RecentPartyTicketMessageIds, *Message) ||
      TicketId.IsEmpty()) {
    return;
  }
  if (TicketId == PendingPartyTicketId) {
    PendingPartyTicketId.Reset();
  }
  // As a ticket error: a player in the world keeps zone transfers through
  // bInWorld.
  if (EndPartyTicket(TicketId)) {
    bAssignmentExpected = false;
  }
}

void URedwoodClientInterface::ForgetPartyTickets() {
  AcceptedPartyTicketId.Reset();
  PendingPartyTicketId.Reset();
  EndedPartyTicketIds.Reset();
  RecentPartyTicketMessageIds.Reset();
}

// A join that the server refused gets no assignment, so a replay of an
// earlier ticket must not move the player. The refusal left no ticket, so no
// leave is owed. Call it before the game sees the reply: the game can join
// again then. Only for the latest join: an older join's refusal must not
// close the guard for a newer one.
void URedwoodClientInterface::NoteJoinAnswered(
  const FString &Error, uint32 Sequence
) {
  if (!Error.IsEmpty() && Sequence == JoinSequence) {
    bAssignmentExpected = false;
  }
}

void URedwoodClientInterface::HandleTicketingJoinReply(
  const FString &Error, uint32 Sequence
) {
  NoteJoinAnswered(Error, Sequence);
  FRedwoodTicketingUpdate Update;
  Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
  Update.Message = Error;
  OnTicketingUpdate.ExecuteIfBound(Update);

  if (!Error.IsEmpty()) {
    OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
  }
}

// A join whose reply was lost: the game gives up on it, so its assignment
// must not move the player, and the ticket or assignment the server may
// have kept is left at the next auth.
void URedwoodClientInterface::NoteJoinLost() {
  bAbandonedQueueJoin = true;
  bAssignmentExpected = false;
  bLeaveTicketingOwed = true;
}

// A proxy join or a create that joins, lost in a drop. Only for the latest
// join, as for a refusal. Before the game sees the failure: it can join
// again then.
TFunction<void()> URedwoodClientInterface::MakeLostProxyJoin(
  uint32 Sequence, TFunction<void()> OnLost
) {
  return [this, Sequence, OnLost = MoveTemp(OnLost)]() {
    if (Sequence == JoinSequence) {
      NoteJoinLost();
    }
    OnLost();
  };
}

// Only for the latest join: a newer join owns the flags and the update.
void URedwoodClientInterface::FailTicketingJoin(uint32 Sequence) {
  if (Sequence != JoinSequence) {
    return;
  }
  NoteJoinLost();
  FRedwoodTicketingUpdate Update;
  Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
  Update.Message = FRedwoodPendingReplies::LostReplyError;
  OnTicketingUpdate.ExecuteIfBound(Update);
  OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
}
// FORK(hollowed-oath) END

void URedwoodClientInterface::Tick(float DeltaTime) {
  TimerManager.Tick(DeltaTime);
}

TStatId URedwoodClientInterface::GetStatId() const {
  RETURN_QUICK_DECLARE_CYCLE_STAT(URedwoodClientInterface, STATGROUP_Tickables);
}

// FORK(hollowed-oath) BEGIN: the one connection. The socket opens here; the
// login joins the session on it. A drop is followed by a reconnect and a
// resume inside the connection; this object hears the outcome and holds the
// requests made in between (HollowedOath#2854).
void URedwoodClientInterface::InitializeDirectorConnection(
  FRedwoodSocketConnectedDelegate OnDirectorConnected
) {
  if (Connection.IsValid()) {
    Connection->Close();
  }
  Connection = MakeShared<FRedwoodBifrostConnection>(TimerManager);
  bSentConnected = false;
  bDisconnected = false;
  bRealmEntered = false;
  bReauthPending = false;
  BindPushHandlers();

  ConnectionUri = URedwoodSettings::GetBifrostUri();
  TWeakObjectPtr<URedwoodClientInterface> WeakThis(this);

  Connection->OnConnected = [WeakThis, OnDirectorConnected]() {
    URedwoodClientInterface *Self = WeakThis.Get();
    if (!Self) {
      return;
    }
    const bool bFirst = !Self->bSentConnected;
    Self->HandleConnected();
    if (bFirst) {
      FRedwoodSocketConnected Details;
      Details.Error = TEXT("");
      OnDirectorConnected.ExecuteIfBound(Details);
    }
  };
  Connection->OnDropped = [WeakThis](bool bAnnounced) {
    // A socket Bifrost closed after `closed` goes with a session this
    // object ended already; nothing is lost that the game must hear of.
    if (!bAnnounced) {
      return;
    }
    if (URedwoodClientInterface *Self = WeakThis.Get()) {
      Self->HandleDropped();
    }
  };
  Connection->OnResumed = [WeakThis](const TSharedPtr<FJsonObject> &Reply) {
    if (URedwoodClientInterface *Self = WeakThis.Get()) {
      Self->HandleResumed(Reply);
    }
  };
  Connection->OnResumeRefused = [WeakThis]() {
    if (URedwoodClientInterface *Self = WeakThis.Get()) {
      Self->HandleResumeRefused();
    }
  };
  Connection->OnClosedByServer = [WeakThis](const FString &Reason) {
    if (URedwoodClientInterface *Self = WeakThis.Get()) {
      Self->HandleClosedByServer(Reason);
    }
  };

  UE_LOG(LogRedwood, Log, TEXT("Connecting to Bifrost at %s"), *ConnectionUri);
  Connection->Connect(ConnectionUri);
}

bool URedwoodClientInterface::IsDirectorConnected() {
  return Connection.IsValid() && Connection->IsConnected();
}

void URedwoodClientInterface::BindPushHandlers() {
  Connection->OnEvent(
    TEXT("player:account-verified"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();
      FString InPlayerId = MessageObject->GetStringField(TEXT("playerId"));

      if (InPlayerId == PlayerId) {
        FRedwoodAuthUpdate Update;
        Update.Type = ERedwoodAuthUpdateType::Success;
        Update.Message = TEXT("");
        OnAccountVerified.ExecuteIfBound(Update);
      }
    }
  );

  // FORK(hollowed-oath) BEGIN: the character friend push. A refused push is
  // logged, so a field the director renames does not stop the feature with no
  // symptom.
  Connection->OnEvent(
    TEXT("director:friends:character-alert"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      FRedwoodCharacterFriendAlert Alert;
      if (!URedwoodCommonGameSubsystem::ParseCharacterFriendAlert(
            Message, Alert
          )) {
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT(
            "Dropped a director:friends:character-alert: the message is not an object, the type is unknown, or a character id is missing. The director and the game may no longer agree on the fields in this message."
          )
        );
        return;
      }

      OnCharacterFriendAlert.Broadcast(Alert);
    }
  );
  // FORK(hollowed-oath) END

  BindRealmPushHandlers();
}

void URedwoodClientInterface::HandleConnected() {
  if (!bSentConnected) {
    bSentConnected = true;
    UE_LOG(LogRedwood, Log, TEXT("Connected to Bifrost at %s"), *ConnectionUri);
    // A first connect that failed broadcast a lost connection, and the game
    // shows a message for it; this is the only event that takes it down.
    if (bLostBeforeFirstConnect) {
      bLostBeforeFirstConnect = false;
      OnConnectionReestablished.Broadcast();
    }
    return;
  }

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Reestablished the connection to Bifrost at %s."),
    *ConnectionUri
  );

  // With a session, the connection resumes it now, and HandleResumed or
  // HandleResumeRefused tells the outcome. The connection counts as lost
  // until then: a resume that this pod refuses drops the socket again, and
  // that is the same outage.
  if (Connection->HasSession()) {
    return;
  }

  // No session to resume: the player was never logged in, logged out during
  // the drop, or a re-login's own socket dropped. The last one logs in again.
  bLoggedOutDuringRelogin = false;
  if (HasPlayerSession()) {
    Relogin();
    return;
  }
  bReauthPending = false;
  // Only a lost connection the game heard of is reestablished: a socket
  // that Bifrost closed after `closed` was not one.
  if (bDisconnected) {
    bDisconnected = false;
    OnConnectionReestablished.Broadcast();
  }
}

void URedwoodClientInterface::HandleDropped() {
  if (!bSentConnected) {
    if (!bSentInitialConnectionFailureLog) {
      bSentInitialConnectionFailureLog = true;
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "Unable to establish the initial connection to Bifrost at %s; will continue to try."
        ),
        *ConnectionUri
      );
    }
    if (!bLostBeforeFirstConnect) {
      bLostBeforeFirstConnect = true;
      OnConnectionLost.Broadcast();
    }
    return;
  }

  // One outage is one drop: the reconnect attempts that fail inside it
  // close their sockets too.
  if (bDisconnected) {
    return;
  }
  bDisconnected = true;

  UE_LOG(
    LogRedwood,
    Error,
    TEXT("Lost the connection to Bifrost at %s; will continue to try to reestablish it."),
    *ConnectionUri
  );

  // HollowedOath#2854. Before bAuthenticated is cleared: it reads whether
  // the player was logged in at the drop.
  bLoggedInAtDrop = bLoggedInAtDrop || bAuthenticated;
  bAuthenticated = false;
  if (HasPlayerSession()) {
    bReauthPending = true;
    // Count the request grace from the drop, like the game's own
    // disconnect grace.
    HeldRequests.StartGrace(TimerManager);
  }
  // HollowedOath#2886. Last, so a request that a failure handler sends again
  // is held for the reconnect.
  Replies.FailAll();
  OnConnectionLost.Broadcast();
}

// The session is back as it was: Redwood saw no drop. The reply says what
// the session has; what this client had and the reply lacks (a realm whose
// start answer was lost, a character whose claim was lost) is sent again.
void URedwoodClientInterface::HandleResumed(
  const TSharedPtr<FJsonObject> &Reply
) {
  // The player logged out while the resume was in flight; its reply brought
  // back a session that must end now. The client's own state is cleared
  // already, so this is the step and the forgetting that Logout would do.
  if (bLoggedOutDuringRelogin) {
    bLoggedOutDuringRelogin = false;
    bReauthPending = false;
    Connection->Step(TEXT("session.logout"), MakeShared<FJsonObject>());
    Connection->ForgetSession();
    return;
  }

  FString RealmId;
  FString LockedRealmId;
  FString CharacterId;
  Reply->TryGetStringField(TEXT("realmId"), RealmId);
  Reply->TryGetStringField(TEXT("lockedRealmId"), LockedRealmId);
  Reply->TryGetStringField(TEXT("characterId"), CharacterId);

  // The realm did not answer the start before the drop. The session is
  // locked to that realm, and no other is accepted, so enter it again: the
  // one this client asked for, or the one Bifrost names when this client's
  // own entry was lost with the socket before it learned the realm.
  if (RealmId.IsEmpty() &&
      (!CurrentRealmId.IsEmpty() || !LockedRealmId.IsEmpty())) {
    if (CurrentRealmId.IsEmpty()) {
      CurrentRealmId = LockedRealmId;
      CurrentRealm.Id = LockedRealmId;
    }
    bRealmEntered = false;
    RestoreRealmAfterRelogin();
    return;
  }

  bRealmEntered = !RealmId.IsEmpty();
  if (bRealmEntered && !SelectedCharacterId.IsEmpty() &&
      (CharacterId != SelectedCharacterId || bSelectCharacterOwedToRealm)) {
    SendSelectCharacter();
  }
  EndReauthentication(true);
}

// The session is gone: its grace ended, or another login replaced it and
// the notice was lost. The token logs the player in again, as before the
// one connection; the new session then enters the realm the old one was in.
void URedwoodClientInterface::HandleResumeRefused() {
  bLoggedOutDuringRelogin = false;
  // The player logged out while the resume was in flight: there is no
  // session to log in again with, and nothing more to do.
  if (!HasPlayerSession()) {
    bReauthPending = false;
    return;
  }
  Relogin();
}

void URedwoodClientInterface::Relogin() {
  UE_LOG(LogRedwood, Log, TEXT("Logging in again after the reconnect."));
  SendLogin(
    PlayerId,
    AuthToken,
    TEXT("local"),
    true,
    FRedwoodAuthUpdateDelegate::CreateLambda(
      [this](const FRedwoodAuthUpdate &Update) {
        if (Update.Type == ERedwoodAuthUpdateType::Success) {
          RestoreRealmAfterRelogin();
          return;
        }
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Could not log in again after the reconnect: %s"),
          *Update.Message
        );
        FailReauthentication(Update.Message);
      }
    )
  );
}

void URedwoodClientInterface::RestoreRealmAfterRelogin() {
  if (CurrentRealmId.IsEmpty()) {
    EndReauthentication(true);
    return;
  }

  EnterRealm(
    CurrentRealmId,
    FRedwoodSocketConnectedDelegate::CreateLambda(
      [this](const FRedwoodSocketConnected &Output) {
        if (!Output.Error.IsEmpty()) {
          UE_LOG(
            LogRedwood,
            Error,
            TEXT("Could not enter the realm again after the reconnect: %s"),
            *Output.Error
          );
          FailReauthentication(Output.Error);
          return;
        }
        // HollowedOath#2448. Before the held requests, which can act for
        // the character.
        if (!SelectedCharacterId.IsEmpty()) {
          SendSelectCharacter();
        }
        EndReauthentication(true);
      }
    )
  );
}

void URedwoodClientInterface::HandleClosedByServer(const FString &Reason) {
  if (Reason == TEXT("replaced")) {
    EndSession(TEXT("This account logged in from another client."));
  }
  // "logout" is this client's own; "reconnect" is followed by the socket
  // close, which HandleDropped sees, and by a resume on a new socket.
}

// Bifrost ended the session, or a re-login could not bring it back. The
// player's requests fail now, and the game hears why.
void URedwoodClientInterface::EndSession(const FString &Message) {
  HeldRequests.Expire(TimerManager);
  Replies.FailAll();
  bReauthPending = false;
  bLoggedOutDuringRelogin = false;
  // The outage, if one was on, ends in this failure, not in a reestablished
  // connection; the next drop is a new one.
  bDisconnected = false;
  ClearSessionState();
  OnAuthFailed.Broadcast(Message);
}

// The session cannot come back: the re-login, or the realm entry after it,
// was refused. It ends as a session Bifrost ended does, so that no later
// socket tries the dead token again and no request waits for it. A new
// session the re-login did make is logged out of Bifrost.
void URedwoodClientInterface::FailReauthentication(const FString &Message) {
  EndReauthentication(false);
  const bool bLoggedOut = bLoggedOutDuringRelogin;
  bLoggedOutDuringRelogin = false;
  bDisconnected = false;
  if (IsDirectorConnected() && Connection->HasSession()) {
    Connection->Step(TEXT("session.logout"), MakeShared<FJsonObject>());
  }
  if (Connection.IsValid()) {
    Connection->ForgetSession();
  }
  ClearSessionState();
  // A player who logged out meanwhile is at the title screen.
  if (!bLoggedOut) {
    OnAuthFailed.Broadcast(Message);
  }
}
// FORK(hollowed-oath) END

void URedwoodClientInterface::HandleRegionsChanged(
  const FString &Event, const TSharedPtr<FJsonValue> &Message
) {
  FRedwoodRegionsChanged MessageStruct;
  USIOJConvert::JsonObjectToUStruct(
    Message->AsObject(),
    FRedwoodRegionsChanged::StaticStruct(),
    &MessageStruct,
    0,
    0
  );

  Regions.Empty();

  for (FRedwoodRegion Region : MessageStruct.Regions) {
    TSharedPtr<FRedwoodRegionLatency> RegionLatency =
      MakeShareable(new FRedwoodRegionLatency);
    RegionLatency->Id = Region.Name;
    RegionLatency->Url = Region.Ping;
    Regions.Add(RegionLatency->Id, RegionLatency);
  }

  TimerManager.ClearTimer(PingTimer);

  URedwoodSettings *RedwoodSettings = GetMutableDefault<URedwoodSettings>();

  PingAttemptsLeft = RedwoodSettings->PingAttempts;

  InitiatePings();
}

void URedwoodClientInterface::InitiatePings() {
  ULatencyCheckerLibrary::FPingResult Delegate;
  Delegate.BindUFunction(this, FName(TEXT("HandlePingResult")));

  URedwoodSettings *RedwoodSettings = GetMutableDefault<URedwoodSettings>();

  int PingAttempts = RedwoodSettings->PingAttempts;

  if (PingAttemptsLeft > 0) {
    for (auto Itr : Regions) {
      if (Itr.Value->Url.StartsWith("ws") && PingAttemptsLeft == PingAttempts) {
        // websockets only need to iterate once as they'll execute
        // multiple ping attempts internally
        PingAttemptsLeft = 1;
      }

      if (Itr.Value->Url.StartsWith("ws") || PingAttemptsLeft == PingAttempts) {
        // clear the last values
        Itr.Value->RTTs.Empty(PingAttempts);
      }

      if (Itr.Value->Url.StartsWith("ws")) {
        ULatencyCheckerLibrary::PingWebSockets(
          Itr.Value->Url, RedwoodSettings->PingTimeout, PingAttempts, Delegate
        );
      } else {
        ULatencyCheckerLibrary::PingIcmp(
          Itr.Value->Url, RedwoodSettings->PingTimeout, Delegate
        );
      }
    }

    PingAttemptsLeft--;
  } else {
    // we're done pinging, let's store the averages
    bool bHasWebsocketRegion = false;
    for (auto Itr : Regions) {
      if (Itr.Value->Url.StartsWith("ws")) {
        bHasWebsocketRegion = true;
      }

      float Minimum = -1;
      for (float RTT : Itr.Value->RTTs) {
        if (Minimum == -1 || RTT < Minimum) {
          Minimum = RTT;
        }
      }

      if (Minimum >= 0) {
        PingAverages.Add(Itr.Key, Minimum);
      }
    }

    OnPingsReceived.Broadcast();

    // queue the next set of pings
    PingAttemptsLeft = bHasWebsocketRegion ? 1 : PingAttempts;

    TimerManager.SetTimer(
      PingTimer,
      this,
      &URedwoodClientInterface::InitiatePings,
      RedwoodSettings->PingFrequency,
      false
    );
  }
}

void URedwoodClientInterface::HandlePingResult(
  FString TargetAddress, float RTT
) {
  URedwoodSettings *RedwoodSettings = GetMutableDefault<URedwoodSettings>();

  for (auto Itr : Regions) {
    if (Itr.Value->Url == TargetAddress) {
      Itr.Value->RTTs.Add(RTT);
      break;
    }
  }

  for (auto Itr : Regions) {
    if (Itr.Value->Url.StartsWith("ws")) {
      // the LatencyChecker module handles averaging for us for websockets
      // and sends PingAttempts and provides a single number
      if (Itr.Value->RTTs.Num() == 0) {
        // we haven't finished receiving all of the pings for this round
        return;
      }
    } else {
      if (Itr.Value->RTTs.Num() + PingAttemptsLeft != RedwoodSettings->PingAttempts) {
        // we haven't finished receiving all of the pings for this round
        return;
      }
    }
  }

  // we've received pings from all Regions, ping again
  InitiatePings();
}

// FORK(hollowed-oath): accounts are made in the launcher (HAMS), and
// Bifrost does not expose the Director's registration.
void URedwoodClientInterface::Register(
  const FString &Username,
  const FString &Password,
  FRedwoodAuthUpdateDelegate OnUpdate
) {
  FRedwoodAuthUpdate Update;
  Update.Type = ERedwoodAuthUpdateType::Error;
  Update.Message = TEXT("Accounts are made in the launcher.");
  OnUpdate.ExecuteIfBound(Update);
}

void URedwoodClientInterface::Logout() {
  // FORK(hollowed-oath): HollowedOath#2854. Not IsLoggedIn: in the silent grace
  // after a drop the player is not authenticated, and a no-op here let the
  // reconnect bring the player back after they chose to leave.
  if (HasPlayerSession()) {
    // Before the ids are cleared, so the held requests fail and are not sent.
    HeldRequests.Expire(TimerManager);
    // Only a resume or re-login still in flight can bring the player back
    // after this; its reply must log out again. A later login is a new
    // session, and must not be logged out for this one.
    bLoggedOutDuringRelogin = bReauthPending;
    bReauthPending = false;

    // Not while a resume is still unanswered: the step would reach no
    // channel. HandleResumed sends the logout when that resume is accepted.
    if (IsDirectorConnected() && Connection->HasSession() &&
        Connection->IsJoined()) {
      // FORK(hollowed-oath): HollowedOath#2886. Only a realm that knows the
      // player can run the leave. In the re-login window the flag is
      // dropped below all the same; the server ticket then expires.
      if (IsRealmReady()) {
        SendOwedLeave();
      }
      // Bifrost ends the session at the realm and the director, and closes
      // the socket; the socket comes back for the next login. A request
      // still waiting is not answered over that close, and the close is one
      // we asked for, so nothing else fails it.
      Connection->Step(TEXT("session.logout"), MakeShared<FJsonObject>());
      Replies.FailAll();
    }
    // A session the socket could not tell is ended after its grace. Either
    // way, no resume must bring it back.
    if (Connection.IsValid()) {
      Connection->ForgetSession();
    }

    ClearSessionState();

    URedwoodSaveGame *SaveGame = Cast<URedwoodSaveGame>(
      UGameplayStatics::CreateSaveGameObject(URedwoodSaveGame::StaticClass())
    );

    UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
  }
}

// FORK(hollowed-oath): what a logout and an end by Bifrost have in common.
void URedwoodClientInterface::ClearSessionState() {
  PlayerId = TEXT("");
  AuthToken = TEXT("");
  // The next session is in no realm until it enters one; a resume of it
  // must not enter the old realm by itself.
  ClearRealmState();
  // The next login can be another account, whose tickets are not ours.
  bLeaveTicketingOwed = false;
  // HollowedOath#2854. See HasPlayerSession: both flags, or a later drop at
  // the title screen logs in again with empty ids and reports an
  // authentication failure there.
  bAuthenticated = false;
  bLoggedInAtDrop = false;
}

bool URedwoodClientInterface::IsLoggedIn() {
  return !PlayerId.IsEmpty() && !AuthToken.IsEmpty() && bAuthenticated;
}

FString URedwoodClientInterface::GetPlayerId() {
  return PlayerId;
}

FString URedwoodClientInterface::GetCharacterId() {
  return SelectedCharacterId;
}

FString URedwoodClientInterface::GetCharacterName() {
  if (!SelectedCharacterId.IsEmpty()) {
    FString *CharacterName = CharacterNamesById.Find(SelectedCharacterId);

    if (CharacterName != nullptr) {
      return *CharacterName;
    }
  }

  return FString();
}

FString URedwoodClientInterface::GetRealmId() {
  return CurrentRealmId;
}

void URedwoodClientInterface::AttemptAutoLogin(
  FRedwoodAuthUpdateDelegate OnUpdate
) {
  if (!IsDirectorConnected()) {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("Not connected to Director.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  URedwoodSaveGame *SaveGame = Cast<URedwoodSaveGame>(
    UGameplayStatics::LoadGameFromSlot(TEXT("RedwoodSaveGame"), 0)
  );

  if (SaveGame && !SaveGame->Username.IsEmpty() && !SaveGame->AuthToken.IsEmpty()) {
    Login(SaveGame->Username, SaveGame->AuthToken, "local", true, OnUpdate);
  } else {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("No saved credentials found.");
    OnUpdate.ExecuteIfBound(Update);
  }
}

void URedwoodClientInterface::Login(
  const FString &Username,
  const FString &PasswordOrToken,
  const FString &Provider,
  bool bRememberMe,
  FRedwoodAuthUpdateDelegate OnUpdate
) {
  // FORK(hollowed-oath): a new session is in no realm, whatever the last one
  // was in; a resume of it must not enter the old realm by itself.
  ClearRealmState();
  SendLogin(Username, PasswordOrToken, Provider, bRememberMe, OnUpdate);
  // A re-login that was still in flight was answered (and silenced) inside
  // SendLogin, so the logout of the old session is over here and must not
  // end the new one.
  bLoggedOutDuringRelogin = false;
}

void URedwoodClientInterface::SendLogin(
  const FString &Username,
  const FString &PasswordOrToken,
  const FString &Provider,
  bool bRememberMe,
  FRedwoodAuthUpdateDelegate OnUpdate
) {
  if (!IsDirectorConnected()) {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("Not connected to Director.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("username"), Username);
  Payload->SetStringField(TEXT("secret"), PasswordOrToken);
  Payload->SetStringField(TEXT("provider"), Provider);

  // FORK(hollowed-oath): the join of the session. Not tracked: the
  // re-login also runs this, and a failure would end the session.
  TWeakObjectPtr<URedwoodClientInterface> WeakThis(this);
  Connection->Login(
    Payload,
    [WeakThis, Username, bRememberMe, OnUpdate](
      bool bOk, const TSharedPtr<FJsonObject> &Response
    ) {
      URedwoodClientInterface *Self = WeakThis.Get();
      if (!Self) {
        return;
      }

      FRedwoodAuthUpdate Update;

      if (bOk) {
        const TSharedPtr<FJsonObject> *Player = nullptr;
        if (Response->TryGetObjectField(TEXT("player"), Player)) {
          (*Player)->TryGetStringField(TEXT("playerId"), Self->PlayerId);
          (*Player)->TryGetStringField(TEXT("token"), Self->AuthToken);
          (*Player)->TryGetStringField(TEXT("nickname"), Self->Nickname);
        }

        Update.Type = ERedwoodAuthUpdateType::Success;
        Update.Message = TEXT("");

        Self->bAuthenticated = true;
        // A new session is in no realm, whatever the last one was in.
        Self->bRealmEntered = false;

        URedwoodSaveGame *SaveGame =
          Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
            URedwoodSaveGame::StaticClass()
          ));

        if (bRememberMe) {
          SaveGame->Username = Username;
          SaveGame->AuthToken = Self->AuthToken;
        }

        UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
      } else {
        FString Reason;
        FString Message;
        Response->TryGetStringField(TEXT("reason"), Reason);
        Response->TryGetStringField(TEXT("message"), Message);

        if (Message == TEXT("Must verify account")) {
          Self->OnAccountVerified = OnUpdate;
          Update.Type = ERedwoodAuthUpdateType::MustVerifyAccount;
          Update.Message = TEXT("");
        } else {
          Update.Type = ERedwoodAuthUpdateType::Error;
          Update.Message =
            Message.IsEmpty() ? JoinRefusalText(Reason) : Message;

          URedwoodSaveGame *SaveGame =
            Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
              URedwoodSaveGame::StaticClass()
            ));
          UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
        }
      }

      OnUpdate.ExecuteIfBound(Update);
    }
  );
}

FString URedwoodClientInterface::GetNickname() {
  return Nickname;
}

void URedwoodClientInterface::CancelWaitingForAccountVerification() {
  if (OnAccountVerified.IsBound()) {
    OnAccountVerified.Unbind();
  }
}

void URedwoodClientInterface::SearchForPlayers(
  FString UsernameOrNickname,
  bool bIncludePartialMatches,
  FRedwoodListPlayersOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SearchForPlayers(UsernameOrNickname, bIncludePartialMatches, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListPlayersOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("searchText"), UsernameOrNickname);
  Payload->SetBoolField(TEXT("includePartial"), bIncludePartialMatches);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:players:search:name"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListPlayersOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Players =
        MessageObject->GetArrayField(TEXT("players"));

      for (TSharedPtr<FJsonValue> InPlayer : Players) {
        FRedwoodPlayer OutPlayer;
        TSharedPtr<FJsonObject> FriendObj = InPlayer->AsObject();
        OutPlayer.PlayerId = FriendObj->GetStringField(TEXT("playerId"));
        OutPlayer.Nickname = FriendObj->GetStringField(TEXT("nickname"));
        OutPlayer.FriendshipState =
          URedwoodCommonGameSubsystem::ParseFriendListType(
            FriendObj->GetStringField(TEXT("friendshipState"))
          );

        const TSharedPtr<FJsonObject> *OnlineStateObj;
        OutPlayer.bOnline =
          FriendObj->TryGetObjectField(TEXT("onlineState"), OnlineStateObj);
        if (OutPlayer.bOnline) {

          const TSharedPtr<FJsonObject> *OnlineStateRealmObj;
          OutPlayer.bPlaying =
            (*OnlineStateObj)
              ->TryGetObjectField(TEXT("realm"), OnlineStateRealmObj);

          if (OutPlayer.bPlaying) {
            OutPlayer.OnlineStateRealm.RealmName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("realmName"));
            OutPlayer.OnlineStateRealm.ProxyId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("proxyId"));
            OutPlayer.OnlineStateRealm.ZoneName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("zoneName"));
            OutPlayer.OnlineStateRealm.ShardName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("shardName"));
            OutPlayer.OnlineStateRealm.CharacterId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("characterId"));
          }
        }

        Output.Players.Add(OutPlayer);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SearchForPlayerById(
  FString TargetPlayerId, FRedwoodPlayerOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SearchForPlayerById(TargetPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodPlayerOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:players:search:id"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodPlayerOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TSharedPtr<FJsonObject> *PlayerObject = nullptr;
      if (MessageObject->TryGetObjectField(TEXT("player"), PlayerObject)) {
        FRedwoodPlayer OutPlayer;
        OutPlayer.PlayerId = (*PlayerObject)->GetStringField(TEXT("playerId"));
        OutPlayer.Nickname = (*PlayerObject)->GetStringField(TEXT("nickname"));
        OutPlayer.FriendshipState =
          URedwoodCommonGameSubsystem::ParseFriendListType(
            (*PlayerObject)->GetStringField(TEXT("friendshipState"))
          );

        const TSharedPtr<FJsonObject> *OnlineStateObj;
        OutPlayer.bOnline =
          (*PlayerObject)
            ->TryGetObjectField(TEXT("onlineState"), OnlineStateObj);
        if (OutPlayer.bOnline) {

          const TSharedPtr<FJsonObject> *OnlineStateRealmObj;
          OutPlayer.bPlaying =
            (*OnlineStateObj)
              ->TryGetObjectField(TEXT("realm"), OnlineStateRealmObj);

          if (OutPlayer.bPlaying) {
            OutPlayer.OnlineStateRealm.RealmName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("realmName"));
            OutPlayer.OnlineStateRealm.ProxyId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("proxyId"));
            OutPlayer.OnlineStateRealm.ZoneName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("zoneName"));
            OutPlayer.OnlineStateRealm.ShardName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("shardName"));
            OutPlayer.OnlineStateRealm.CharacterId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("characterId"));
          }
        }

        Output.Player = OutPlayer;
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListFriends(
  ERedwoodFriendListType Filter, FRedwoodListPlayersOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListFriends(Filter, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListPlayersOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  if (Filter == ERedwoodFriendListType::All || Filter == ERedwoodFriendListType::Unknown) {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("filter"), NullValue);
  } else {
    Payload->SetStringField(TEXT("filter"), RW_ENUM_TO_STRING(Filter));
  }

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:friends:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListPlayersOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Players =
        MessageObject->GetArrayField(TEXT("players"));

      for (TSharedPtr<FJsonValue> InPlayer : Players) {
        FRedwoodPlayer OutPlayer;
        TSharedPtr<FJsonObject> FriendObj = InPlayer->AsObject();
        OutPlayer.PlayerId = FriendObj->GetStringField(TEXT("playerId"));
        OutPlayer.Nickname = FriendObj->GetStringField(TEXT("nickname"));
        OutPlayer.FriendshipState =
          URedwoodCommonGameSubsystem::ParseFriendListType(
            FriendObj->GetStringField(TEXT("friendshipState"))
          );

        const TSharedPtr<FJsonObject> *OnlineStateObj;
        OutPlayer.bOnline =
          FriendObj->TryGetObjectField(TEXT("onlineState"), OnlineStateObj);
        if (OutPlayer.bOnline) {

          const TSharedPtr<FJsonObject> *OnlineStateRealmObj;
          OutPlayer.bPlaying =
            (*OnlineStateObj)
              ->TryGetObjectField(TEXT("realm"), OnlineStateRealmObj);

          if (OutPlayer.bPlaying) {
            OutPlayer.OnlineStateRealm.RealmName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("realmName"));
            OutPlayer.OnlineStateRealm.ProxyId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("proxyId"));
            OutPlayer.OnlineStateRealm.ZoneName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("zoneName"));
            OutPlayer.OnlineStateRealm.ShardName =
              (*OnlineStateRealmObj)->GetStringField(TEXT("shardName"));
            OutPlayer.OnlineStateRealm.CharacterId =
              (*OnlineStateRealmObj)->GetStringField(TEXT("characterId"));
          }
        }

        Output.Players.Add(OutPlayer);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::RequestFriend(
  FString OtherPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        RequestFriend(OtherPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:friends:add"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::RemoveFriend(
  FString OtherPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        RemoveFriend(OtherPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:friends:remove"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::RespondToFriendRequest(
  FString OtherPlayerId, bool bAccept, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        RespondToFriendRequest(OtherPlayerId, bAccept, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);
  Payload->SetBoolField(TEXT("accept"), bAccept);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:friends:respond-request"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetPlayerBlocked(
  FString OtherPlayerId, bool bBlocked, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SetPlayerBlocked(OtherPlayerId, bBlocked, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);
  Payload->SetBoolField(TEXT("block"), bBlocked);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:friends:block"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListRealmContacts(
  FRedwoodListRealmContactsOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        ListRealmContacts(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodListRealmContactsOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:contacts:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListRealmContactsOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      if (Output.Error.IsEmpty()) {
        const TArray<TSharedPtr<FJsonValue>> *ContactsArray;
        if (MessageObject->TryGetArrayField(TEXT("contacts"), ContactsArray)) {
          for (const TSharedPtr<FJsonValue> &ContactValue : *ContactsArray) {
            TSharedPtr<FJsonObject> ContactInfoObject =
              ContactValue->AsObject();

            FRedwoodRealmContact Contact;
            Contact.CharacterId =
              ContactInfoObject->GetStringField(TEXT("characterId"));
            Contact.CharacterName =
              ContactInfoObject->GetStringField(TEXT("characterName"));
            Contact.Description =
              ContactInfoObject->GetStringField(TEXT("description"));

            Output.Contacts.Add(Contact);
          }
        }

        if (MessageObject->TryGetArrayField(
              TEXT("blockedContacts"), ContactsArray
            )) {
          for (const TSharedPtr<FJsonValue> &ContactValue : *ContactsArray) {
            TSharedPtr<FJsonObject> ContactInfoObject =
              ContactValue->AsObject();

            FRedwoodRealmContact Contact;
            Contact.CharacterId =
              ContactInfoObject->GetStringField(TEXT("characterId"));
            Contact.CharacterName =
              ContactInfoObject->GetStringField(TEXT("characterName"));
            Contact.Description =
              ContactInfoObject->GetStringField(TEXT("description"));

            Output.BlockedContacts.Add(Contact);
          }
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::AddRealmContact(
  FString OtherCharacterId, bool bBlocked, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        AddRealmContact(OtherCharacterId, bBlocked, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetStringField(TEXT("targetCharacterId"), OtherCharacterId);
  Payload->SetBoolField(TEXT("blocked"), bBlocked);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:contacts:add"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::RemoveRealmContact(
  FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        RemoveRealmContact(OtherCharacterId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetStringField(TEXT("targetCharacterId"), OtherCharacterId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:contacts:remove"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

// FORK(hollowed-oath) BEGIN: character friend calls.
namespace {
  const TCHAR *const NoCharacterSelectedError = TEXT("No character selected.");

  // A reply with no error field is not a success: it is a bad answer.
  FString ParseCharacterFriendCommandAnswer(
    const TArray<TSharedPtr<FJsonValue>> &Response
  ) {
    const TSharedPtr<FJsonObject> *MessageObject =
      URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
    FString Error;
    if (MessageObject == nullptr ||
        !URedwoodCommonGameSubsystem::TryGetRedwoodStringField(
          *MessageObject, TEXT("error"), Error
        )) {
      return URedwoodCommonGameSubsystem::BadRealmAnswerError;
    }
    return Error;
  }
}

template <typename TOutput>
void URedwoodClientInterface::EmitCharacterFriendCall(
  const FString &EventName,
  const FString &CharacterId,
  TSharedPtr<FJsonObject> Payload,
  TOutput (*ParseAnswer)(const TArray<TSharedPtr<FJsonValue>> &),
  const TDelegate<void(const TOutput &)> &OnOutput
) {
  if (GateRealm([=, this]() {
        // The backend accepts any character of the player, and a list answer
        // names no character, so a stale call would act for the wrong one.
        if (CharacterId != SelectedCharacterId) {
          TOutput Output;
          SetRedwoodGateError(
            Output, URedwoodCommonGameSubsystem::CharacterChangedError
          );
          OnOutput.ExecuteIfBound(Output);
          return;
        }
        EmitCharacterFriendCall(
          EventName, CharacterId, Payload, ParseAnswer, OnOutput
        );
      }, OnOutput)) {
    return;
  }

  if (CharacterId.IsEmpty()) {
    TOutput Output;
    SetRedwoodGateError(Output, NoCharacterSelectedError);
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);
  // HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    EventName,
    Payload,
    TrackReply(
      [this, CharacterId, ParseAnswer, OnOutput](auto Response) {
        // A list answer names no character, so after a switch it would fill
        // the new character's friends with the old one's.
        if (CharacterId != SelectedCharacterId) {
          TOutput Output;
          SetRedwoodGateError(
            Output, URedwoodCommonGameSubsystem::CharacterChangedError
          );
          OnOutput.ExecuteIfBound(Output);
          return;
        }
        OnOutput.ExecuteIfBound(ParseAnswer(Response));
      },
      OnOutput
    )
  );
}

void URedwoodClientInterface::ListCharacterFriends(
  FRedwoodListCharacterFriendsOutputDelegate OnOutput
) {
  EmitCharacterFriendCall(
    TEXT("realm:contacts:list"),
    SelectedCharacterId,
    MakeShared<FJsonObject>(),
    &URedwoodCommonGameSubsystem::ParseListCharacterFriends,
    OnOutput
  );
}

void URedwoodClientInterface::RequestCharacterFriend(
  FString TargetCharacterId, FRedwoodErrorOutputDelegate OnOutput
) {
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField(TEXT("targetCharacterId"), TargetCharacterId);
  EmitCharacterFriendCall(
    TEXT("realm:contacts:friends:request"),
    SelectedCharacterId,
    Payload,
    &ParseCharacterFriendCommandAnswer,
    OnOutput
  );
}

void URedwoodClientInterface::RespondToCharacterFriendRequest(
  FString OtherCharacterId, bool bAccept, FRedwoodErrorOutputDelegate OnOutput
) {
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField(TEXT("otherCharacterId"), OtherCharacterId);
  Payload->SetBoolField(TEXT("accept"), bAccept);
  EmitCharacterFriendCall(
    TEXT("realm:contacts:friends:respond"),
    SelectedCharacterId,
    Payload,
    &ParseCharacterFriendCommandAnswer,
    OnOutput
  );
}

void URedwoodClientInterface::RemoveCharacterFriend(
  FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
) {
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField(TEXT("otherCharacterId"), OtherCharacterId);
  EmitCharacterFriendCall(
    TEXT("realm:contacts:friends:remove"),
    SelectedCharacterId,
    Payload,
    &ParseCharacterFriendCommandAnswer,
    OnOutput
  );
}

void URedwoodClientInterface::CancelCharacterFriendRequest(
  FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
) {
  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField(TEXT("otherCharacterId"), OtherCharacterId);
  Payload->SetBoolField(TEXT("requestOnly"), true);
  EmitCharacterFriendCall(
    TEXT("realm:contacts:friends:remove"),
    SelectedCharacterId,
    Payload,
    &ParseCharacterFriendCommandAnswer,
    OnOutput
  );
}
// FORK(hollowed-oath) END

void URedwoodClientInterface::ListGuilds(
  bool bOnlyPlayersGuilds, FRedwoodListGuildsOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListGuilds(bOnlyPlayersGuilds, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListGuildsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetBoolField(TEXT("onlyPlayersGuilds"), bOnlyPlayersGuilds);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListGuildsOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      if (Output.Error.IsEmpty()) {
        const TArray<TSharedPtr<FJsonValue>> *GuildsArray;
        if (MessageObject->TryGetArrayField(TEXT("guilds"), GuildsArray)) {
          for (const TSharedPtr<FJsonValue> &GuildValue : *GuildsArray) {
            TSharedPtr<FJsonObject> GuildInfoObject = GuildValue->AsObject();

            Output.Guilds.Add(
              URedwoodCommonGameSubsystem::ParseGuildInfo(GuildInfoObject)
            );
          }
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SearchForGuilds(
  FString SearchText,
  bool bIncludePartialMatches,
  FRedwoodListGuildsOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SearchForGuilds(SearchText, bIncludePartialMatches, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListGuildsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("searchText"), SearchText);
  Payload->SetBoolField(TEXT("includePartial"), bIncludePartialMatches);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:search"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListGuildsOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      if (Output.Error.IsEmpty()) {
        const TArray<TSharedPtr<FJsonValue>> *GuildsArray;
        if (MessageObject->TryGetArrayField(TEXT("guilds"), GuildsArray)) {
          for (const TSharedPtr<FJsonValue> &GuildValue : *GuildsArray) {
            TSharedPtr<FJsonObject> GuildInfoObject = GuildValue->AsObject();

            Output.Guilds.Add(
              URedwoodCommonGameSubsystem::ParseGuildInfo(GuildInfoObject)
            );
          }
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::GetGuild(
  FString GuildId, FRedwoodGetGuildOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        GetGuild(GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodGetGuildOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:get"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodGetGuildOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      if (Output.Error.IsEmpty()) {
        TSharedPtr<FJsonObject> GuildObject =
          MessageObject->GetObjectField(TEXT("guild"));
        if (GuildObject) {
          Output.Guild =
            URedwoodCommonGameSubsystem::ParseGuildInfo(GuildObject);
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::GetSelectedGuild(
  FRedwoodGetGuildOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        GetSelectedGuild(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodGetGuildOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:selected:get"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodGetGuildOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      if (Output.Error.IsEmpty()) {
        TSharedPtr<FJsonObject> GuildObject =
          MessageObject->GetObjectField(TEXT("guild"));
        if (GuildObject) {
          Output.Guild =
            URedwoodCommonGameSubsystem::ParseGuildInfo(GuildObject);
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetSelectedGuild(
  FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SetSelectedGuild(GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:selected:set"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::JoinGuild(
  FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        JoinGuild(GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:membership:join"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::InviteToGuild(
  FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        InviteToGuild(GuildId, TargetId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:membership:invite"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::LeaveGuild(
  FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        LeaveGuild(GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:membership:leave"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListGuildMembers(
  FString GuildId,
  ERedwoodGuildAndAllianceMemberState State,
  FRedwoodListGuildMembersOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListGuildMembers(GuildId, State, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListGuildMembersOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(
    TEXT("state"),
    URedwoodCommonGameSubsystem::SerializeGuildAndAllianceMemberState(State)
  );

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:membership:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListGuildMembersOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Members =
        MessageObject->GetArrayField(TEXT("members"));

      for (TSharedPtr<FJsonValue> InMember : Members) {
        FRedwoodGuildPlayerMembership OutMember;
        TSharedPtr<FJsonObject> MemberObj = InMember->AsObject();
        if (!MemberObj.IsValid()) {
          continue; // skip invalid members
        }
        TSharedPtr<FJsonObject> PlayerObj =
          MemberObj->GetObjectField(TEXT("player"));
        if (!PlayerObj.IsValid()) {
          continue; // skip members without player info
        }
        OutMember.Player.Id = PlayerObj->GetStringField(TEXT("id"));
        OutMember.Player.Name = PlayerObj->GetStringField(TEXT("name"));
        OutMember.PlayerState =
          URedwoodCommonGameSubsystem::ParseGuildAndAllianceMemberState(
            MemberObj->GetStringField(TEXT("playerState"))
          );

        Output.Members.Add(OutMember);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::CreateGuild(
  FString GuildName,
  FString GuildTag,
  ERedwoodGuildInviteType InviteType,
  bool bListed,
  bool bMembershipPublic,
  FRedwoodCreateGuildOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        CreateGuild(
          GuildName, GuildTag, InviteType, bListed, bMembershipPublic, OnOutput
        );
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodCreateGuildOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("name"), GuildName);
  Payload->SetStringField(TEXT("tag"), GuildTag);
  Payload->SetStringField(
    TEXT("inviteType"),
    URedwoodCommonGameSubsystem::SerializeGuildInviteType(InviteType)
  );
  Payload->SetBoolField(TEXT("listed"), bListed);
  Payload->SetBoolField(TEXT("membershipPublic"), bMembershipPublic);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:create"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodCreateGuildOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));
      MessageObject->TryGetStringField(TEXT("guildId"), Output.GuildId);

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::UpdateGuild(
  FString GuildId,
  FString GuildName,
  FString GuildTag,
  ERedwoodGuildInviteType InviteType,
  bool bListed,
  bool bMembershipPublic,
  FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        UpdateGuild(
          GuildId,
          GuildName,
          GuildTag,
          InviteType,
          bListed,
          bMembershipPublic,
          OnOutput
        );
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("name"), GuildName);
  Payload->SetStringField(TEXT("tag"), GuildTag);
  Payload->SetStringField(
    TEXT("inviteType"),
    URedwoodCommonGameSubsystem::SerializeGuildInviteType(InviteType)
  );
  Payload->SetBoolField(TEXT("listed"), bListed);
  Payload->SetBoolField(TEXT("membershipPublic"), bMembershipPublic);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:update"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::KickPlayerFromGuild(
  FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        KickPlayerFromGuild(GuildId, TargetId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);
  Payload->SetBoolField(TEXT("ban"), false);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:kick"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::BanPlayerFromGuild(
  FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        BanPlayerFromGuild(GuildId, TargetId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);
  Payload->SetBoolField(TEXT("ban"), true);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:kick"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::UnbanPlayerFromGuild(
  FString GuildId, FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  KickPlayerFromGuild(GuildId, TargetPlayerId, OnOutput);
}

void URedwoodClientInterface::PromotePlayerToGuildAdmin(
  FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        PromotePlayerToGuildAdmin(GuildId, TargetId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:promote"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::DemotePlayerFromGuildAdmin(
  FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        DemotePlayerFromGuildAdmin(GuildId, TargetId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:admin:demote"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListAlliances(
  FString GuildIdFilter, FRedwoodListAlliancesOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListAlliances(GuildIdFilter, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListAlliancesOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildIdFilter"), GuildIdFilter);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListAlliancesOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Alliances =
        MessageObject->GetArrayField(TEXT("alliances"));

      for (TSharedPtr<FJsonValue> InAlliance : Alliances) {
        TSharedPtr<FJsonObject> AllianceObj = InAlliance->AsObject();

        Output.Alliances.Add(
          URedwoodCommonGameSubsystem::ParseAlliance(AllianceObj)
        );
      }

      // check if guildStates is not null
      const TArray<TSharedPtr<FJsonValue>> *GuildStates;
      if (MessageObject->TryGetArrayField(TEXT("guildStates"), GuildStates)) {
        for (const TSharedPtr<FJsonValue> &InState : *GuildStates) {
          Output.GuildStates.Add(
            URedwoodCommonGameSubsystem::ParseGuildAndAllianceMemberState(
              InState->AsString()
            )
          );
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SearchForAlliances(
  FString SearchText,
  bool bIncludePartialMatches,
  FRedwoodListAlliancesOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        SearchForAlliances(SearchText, bIncludePartialMatches, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListAlliancesOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("searchText"), SearchText);
  Payload->SetBoolField(TEXT("includePartial"), bIncludePartialMatches);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:search"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListAlliancesOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Alliances =
        MessageObject->GetArrayField(TEXT("alliances"));

      for (TSharedPtr<FJsonValue> InAlliance : Alliances) {
        TSharedPtr<FJsonObject> AllianceObj = InAlliance->AsObject();

        Output.Alliances.Add(
          URedwoodCommonGameSubsystem::ParseAlliance(AllianceObj)
        );
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::CanAdminAlliance(
  FString AllianceId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        CanAdminAlliance(AllianceId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Director."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:admin:has-admin-privileges"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::CreateAlliance(
  FString AllianceName,
  FString GuildId,
  bool bInviteOnly,
  FRedwoodCreateAllianceOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        CreateAlliance(AllianceName, GuildId, bInviteOnly, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodCreateAllianceOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("name"), AllianceName);
  Payload->SetBoolField(TEXT("inviteOnly"), bInviteOnly);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:admin:create"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodCreateAllianceOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));
      MessageObject->TryGetStringField(TEXT("allianceId"), Output.AllianceId);

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::UpdateAlliance(
  FString AllianceId,
  FString AllianceName,
  bool bInviteOnly,
  FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        UpdateAlliance(AllianceId, AllianceName, bInviteOnly, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("name"), AllianceName);
  Payload->SetBoolField(TEXT("inviteOnly"), bInviteOnly);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:admin:update"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::KickGuildFromAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        KickGuildFromAlliance(AllianceId, GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("targetGuildId"), GuildId);
  Payload->SetBoolField(TEXT("ban"), false);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:admin:kick"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::BanGuildFromAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        BanGuildFromAlliance(AllianceId, GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetBoolField(TEXT("ban"), true);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:admin:kick"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::UnbanGuildFromAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  KickGuildFromAlliance(AllianceId, GuildId, OnOutput);
}

void URedwoodClientInterface::ListAllianceGuilds(
  FString AllianceId,
  ERedwoodGuildAndAllianceMemberState State,
  FRedwoodListAllianceGuildsOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListAllianceGuilds(AllianceId, State, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodListAllianceGuildsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(
    TEXT("state"),
    URedwoodCommonGameSubsystem::SerializeGuildAndAllianceMemberState(State)
  );

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:membership:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListAllianceGuildsOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Guilds =
        MessageObject->GetArrayField(TEXT("guilds"));

      for (TSharedPtr<FJsonValue> InGuild : Guilds) {
        FRedwoodAllianceGuildMembership OutGuild;
        TSharedPtr<FJsonObject> GuildMembershipObj = InGuild->AsObject();
        if (!GuildMembershipObj.IsValid()) {
          continue; // skip invalid guilds
        }
        TSharedPtr<FJsonObject> GuildObj =
          GuildMembershipObj->GetObjectField(TEXT("guild"));
        if (!GuildObj.IsValid()) {
          continue; // skip guilds without player info
        }
        OutGuild.Guild = URedwoodCommonGameSubsystem::ParseGuild(GuildObj);

        OutGuild.GuildState =
          URedwoodCommonGameSubsystem::ParseGuildAndAllianceMemberState(
            GuildMembershipObj->GetStringField(TEXT("guildState"))
          );

        Output.Guilds.Add(OutGuild);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::JoinAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        JoinAlliance(AllianceId, GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:membership:join"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::LeaveAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        LeaveAlliance(AllianceId, GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:membership:leave"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::InviteGuildToAlliance(
  FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        InviteGuildToAlliance(AllianceId, GuildId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("targetGuildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:guilds:alliances:membership:invite"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListRealms(
  FRedwoodListRealmsOutputDelegate OnOutput
) {
  if (!IsDirectorConnected()) {
    FRedwoodListRealmsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodListRealmsOutput Output;

      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TArray<TSharedPtr<FJsonValue>> &Realms =
        MessageObject->GetArrayField(TEXT("realms"));

      // FORK(hollowed-oath): the loop body moved into ParseRealm so a test can reach it.
      for (TSharedPtr<FJsonValue> InRealm : Realms) {
        Output.Realms.Add(ParseRealm(InRealm->AsObject()));
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

FRedwoodRealm URedwoodClientInterface::ParseRealm(
  const TSharedPtr<FJsonObject> &RealmObj
) {
  FRedwoodRealm OutRealm;
  OutRealm.Id = RealmObj->GetStringField(TEXT("id"));
  FDateTime::ParseIso8601(
    *RealmObj->GetStringField(TEXT("createdAt")), OutRealm.CreatedAt
  );
  FDateTime::ParseIso8601(
    *RealmObj->GetStringField(TEXT("updatedAt")), OutRealm.UpdatedAt
  );
  OutRealm.Name = RealmObj->GetStringField(TEXT("name"));
  OutRealm.Uri = RealmObj->GetStringField(TEXT("uri"));
  OutRealm.bListed = RealmObj->GetBoolField(TEXT("listed"));
  OutRealm.Secret = RealmObj->GetStringField(TEXT("secret"));
  // FORK(hollowed-oath): an older director sends no version. TryGet keeps it
  // empty and does not log the error that GetStringField writes.
  RealmObj->TryGetStringField(TEXT("version"), OutRealm.Version);
  return OutRealm;
}

void URedwoodClientInterface::InitializeConnectionForFirstRealm(
  FRedwoodSocketConnectedDelegate OnRealmConnected
) {
  ListRealms(FRedwoodListRealmsOutputDelegate::CreateLambda(
    [this, OnRealmConnected](const FRedwoodListRealmsOutput &Output) {
      if (!Output.Error.IsEmpty()) {
        FRedwoodSocketConnected ConnectionResult;
        ConnectionResult.Error = Output.Error;
        OnRealmConnected.ExecuteIfBound(ConnectionResult);
        return;
      }

      if (Output.Realms.Num() == 0) {
        FRedwoodSocketConnected ConnectionResult;
        ConnectionResult.Error = TEXT("No realms found.");
        OnRealmConnected.ExecuteIfBound(ConnectionResult);
        return;
      }

      InitializeRealmConnection(
        Output.Realms[0],
        FRedwoodSocketConnectedDelegate::CreateLambda(
          [OnRealmConnected](const FRedwoodSocketConnected &ConnectionResult) {
            OnRealmConnected.ExecuteIfBound(ConnectionResult);
          }
        )
      );
    }
  ));
}

// FORK(hollowed-oath): the realm handshake of upstream (a token from the
// Director, a second socket to the Realm) is one step of the session:
// Bifrost asks the realm to start the session, and the realm asks the
// Director to admit it. A session is in one realm; to change realm, the
// client logs in again.
void URedwoodClientInterface::InitializeRealmConnection(
  FRedwoodRealm InRealm, FRedwoodSocketConnectedDelegate OnRealmConnected
) {
  if (!IsDirectorConnected() || !IsLoggedIn()) {
    FRedwoodSocketConnected Output;
    Output.Error = TEXT("Not connected or authenticated to Director.");
    OnRealmConnected.ExecuteIfBound(Output);
    return;
  }

  // FORK(hollowed-oath): the session is in this realm already (the game asks
  // again after an entry whose answer was lost with the socket, and the
  // resume entered it). A second start at the realm would replace the
  // session with itself, so the answer is given here.
  if (bRealmEntered && CurrentRealmId == InRealm.Id) {
    CurrentRealm = InRealm;
    FRedwoodSocketConnected Output;
    Output.Error = TEXT("");
    OnRealmConnected.ExecuteIfBound(Output);
    return;
  }

  ClearRealmState();
  // FORK(hollowed-oath): HollowedOath#2854. Requests held for the old realm
  // cannot go to this one, so they fail now.
  HeldRequests.Expire(TimerManager);

  CurrentRealm = InRealm;
  EnterRealm(InRealm.Id, OnRealmConnected);
}

// FORK(hollowed-oath): what a session has in a realm. Cleared by a new
// entry, a new login and the end of the session, so that nothing of the old
// realm (its character, its party, a kept zone assignment) acts in the new.
void URedwoodClientInterface::ClearRealmState() {
  CurrentRealm = FRedwoodRealm();
  CurrentRealmId = TEXT("");
  bRealmEntered = false;
  // HollowedOath#2886. The server can replay an assignment kept for the
  // last character; nothing here may accept it.
  SelectedCharacterId = TEXT("");
  bSelectCharacterOwedToRealm = false;
  bAssignmentExpected = false;
  bInWorld = false;
  bAbandonedQueueJoin = false;
  bTravelPending = false;
  // The party of another session or realm; the Realm sends ours again.
  CurrentParty = FRedwoodParty();
  ForgetPartyTickets();
}

void URedwoodClientInterface::EnterRealm(
  const FString &RealmId, FRedwoodSocketConnectedDelegate OnEntered
) {
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("realmId"), RealmId);

  UE_LOG(LogRedwood, Log, TEXT("Entering realm %s"), *RealmId);

  // Not tracked (HA plan ruling C1): the reconnect also runs this, and a
  // failure would end the session.
  TWeakObjectPtr<URedwoodClientInterface> WeakThis(this);
  Connection->Step(
    TEXT("session.enter-realm"),
    Payload,
    [WeakThis, RealmId, OnEntered](auto Response) {
      URedwoodClientInterface *Self = WeakThis.Get();
      if (!Self) {
        return;
      }
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        Self->CurrentRealmId = RealmId;
        Self->bRealmEntered = true;
        Self->ApplyRegions(MessageObject);
        // HollowedOath#2886. Before the caller can join.
        Self->SendOwedLeave();
      }

      FRedwoodSocketConnected Output;
      Output.Error = Error;
      OnEntered.ExecuteIfBound(Output);
    }
  );
}

// The realm's start answer carries its regions, where upstream's realm
// socket pushed them at the connect.
void URedwoodClientInterface::ApplyRegions(
  const TSharedPtr<FJsonObject> &Response
) {
  if (Response->HasTypedField<EJson::Array>(TEXT("regions"))) {
    HandleRegionsChanged(
      TEXT("realm:regions"), MakeShared<FJsonValueObject>(Response)
    );
  }
}

// FORK(hollowed-oath): the realm's pushes, bound once on the one connection.
void URedwoodClientInterface::BindRealmPushHandlers() {
  Connection->OnEvent(
    TEXT("realm:regions"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      HandleRegionsChanged(Event, Message);
    }
  );

  Connection->OnEvent(
    TEXT("realm:ticketing:update"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::Update;
      Update.Message = Message->AsObject()->GetStringField(TEXT("message"));
      OnTicketingUpdate.ExecuteIfBound(Update);
    }
  );

  Connection->OnEvent(
    TEXT("realm:ticketing:ticket-error"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::TicketError;
      Update.Message = Message->AsObject()->GetStringField(TEXT("error"));
      // FORK(hollowed-oath): HollowedOath#2886. The ticket ended, so no
      // assignment comes for it; a replay of an earlier one must not move
      // the player. No ticket is left, so no leave is owed.
      bAssignmentExpected = false;
      OnTicketingUpdate.ExecuteIfBound(Update);

      OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
    }
  );

  // FORK(hollowed-oath) BEGIN: HollowedOath#3002.
  Connection->OnEvent(
    TEXT("realm:ticketing:party-queued"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      HandlePartyQueued(Message->AsObject());
    }
  );

  Connection->OnEvent(
    TEXT("realm:ticketing:party-left"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      HandlePartyLeft(Message->AsObject());
    }
  );
  // FORK(hollowed-oath) END

  Connection->OnEvent(
    TEXT("realm:servers:connect-to-instance"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      // FORK(hollowed-oath): HollowedOath#2886. A zone assignment sent again
      // after a reconnect can be for a request the game dropped, for a
      // character the player no longer plays, or reach a player who is at
      // character select with no join. None may move the player.
      // An old server sends no characterId; its assignment moves as before.
      FString AssignedCharacterId;
      MessageObject->TryGetStringField(TEXT("characterId"), AssignedCharacterId);
      const bool bForAnotherCharacter = !AssignedCharacterId.IsEmpty() &&
        AssignedCharacterId != SelectedCharacterId;
      // HollowedOath#3002. A party ticket's assignment names it. A member
      // that the server told of a party ticket takes only that ticket's
      // assignment; in the world, it still takes a zone transfer with no
      // ticket. An ended ticket has no valid assignment left.
      FString AssignedTicketId;
      MessageObject->TryGetStringField(TEXT("ticketId"), AssignedTicketId);
      const bool bNotThePartyTicket = !AcceptedPartyTicketId.IsEmpty() &&
        (AssignedTicketId.IsEmpty() ? !bInWorld
                                    : AssignedTicketId != AcceptedPartyTicketId);
      const bool bEndedTicket = !AssignedTicketId.IsEmpty() &&
        EndedPartyTicketIds.Contains(AssignedTicketId);
      if ((!bAssignmentExpected && !bInWorld) || bAbandonedQueueJoin ||
          bForAnotherCharacter || bNotThePartyTicket || bEndedTicket) {
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT(
            "Ignoring a zone assignment for a dropped queue request or another character."
          )
        );
        return;
      }

      // FORK(hollowed-oath): HollowedOath#2886. See NoteArrivedInWorld.
      bTravelPending = true;
      TravelJoinSequence = JoinSequence;
      // The ticket is used: a "queued" for it sent again must not arm the
      // guard for a replay of this assignment.
      if (!AssignedTicketId.IsEmpty()) {
        EndPartyTicket(AssignedTicketId);
      }

      bool bShouldStitch = MessageObject->GetBoolField(TEXT("shouldStitch"));
      ServerConnection = MessageObject->GetStringField(TEXT("connection"));
      ServerToken = MessageObject->GetStringField(TEXT("token"));

      if (bShouldStitch) {
        FURL URL = GetConnectionURL();
        OnRequestToStitchServer.Broadcast(URL);
      } else {
        FString ConsoleCommand = GetConnectionConsoleCommand();
        OnRequestToJoinServer.Broadcast(ConsoleCommand);
      }

      OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
    }
  );

  Connection->OnEvent(
    TEXT("realm:parties:invites:alert"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      TSharedPtr<FJsonObject> InviteObject =
        MessageObject->GetObjectField(TEXT("invite"));
      FRedwoodPartyInvite Invite =
        URedwoodCommonGameSubsystem::ParsePartyInvite(InviteObject);

      OnPartyInvited.Broadcast(Invite);
    }
  );

  Connection->OnEvent(
    TEXT("realm:parties:changed"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      TSharedPtr<FJsonObject> PartyObject =
        MessageObject->GetObjectField(TEXT("party"));
      // FORK(hollowed-oath): HollowedOath#2448. Upstream held and broadcast
      // every roster. See ApplyPartyChange.
      const FRedwoodParty Parsed =
        URedwoodCommonGameSubsystem::ParseParty(PartyObject);
      if (ApplyPartyChange(CurrentParty, Parsed)) {
        OnPartyUpdated.Broadcast(Parsed);
      }
    }
  );

  Connection->OnEvent(
    TEXT("realm:parties:kick"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      CurrentParty = FRedwoodParty();
      OnPartyKicked.Broadcast();
    }
  );

  Connection->OnEvent(
    TEXT("realm:parties:emote"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      FString TargetPlayerId = MessageObject->GetStringField(TEXT("playerId"));
      FString Emote = MessageObject->GetStringField(TEXT("emote"));

      OnPartyEmoteReceived.Broadcast(TargetPlayerId, Emote);
    }
  );
}

bool URedwoodClientInterface::IsRealmConnected(FRedwoodRealm &OutRealm) {
  bool bIsConnected = IsRealmConnected();
  if (bIsConnected) {
    OutRealm = CurrentRealm;
  }
  return bIsConnected;
}

bool URedwoodClientInterface::IsRealmConnected() {
  return IsDirectorConnected() && bRealmEntered;
}

// FORK(hollowed-oath): HollowedOath#2886. For the requests the gates do not
// hold. In the re-login window the socket is back but does not know the
// player, and nothing answers a request sent to it.
bool URedwoodClientInterface::IsRealmReady() {
  return IsRealmConnected() && !bReauthPending;
}

TMap<FString, float> URedwoodClientInterface::GetRegions() {
  return PingAverages;
}

void URedwoodClientInterface::ListCharacters(
  FRedwoodListCharactersOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        ListCharacters(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodListCharactersOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      TArray<TSharedPtr<FJsonValue>> Characters =
        MessageObject->GetArrayField(TEXT("characters"));

      CharacterNamesById.Reset();
      TArray<FRedwoodCharacterBackend> CharactersStruct;
      for (TSharedPtr<FJsonValue> Character : Characters) {
        TSharedPtr<FJsonObject> CharacterData = Character->AsObject();

        FRedwoodCharacterBackend CharacterStruct =
          URedwoodCommonGameSubsystem::ParseCharacter(CharacterData);

        CharactersStruct.Add(CharacterStruct);

        CharacterNamesById.Add(CharacterStruct.Id, CharacterStruct.Name);
      }

      FRedwoodListCharactersOutput Output;
      Output.Error = Error;
      Output.Characters = CharactersStruct;
      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListArchivedCharacters(
  FRedwoodListCharactersOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        ListArchivedCharacters(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodListCharactersOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetBoolField(TEXT("includeArchived"), true);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:list"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      TArray<TSharedPtr<FJsonValue>> Characters =
        MessageObject->GetArrayField(TEXT("characters"));

      TArray<FRedwoodCharacterBackend> CharactersStruct;
      for (TSharedPtr<FJsonValue> Character : Characters) {
        TSharedPtr<FJsonObject> CharacterData = Character->AsObject();

        FRedwoodCharacterBackend ParsedCharacter =
          URedwoodCommonGameSubsystem::ParseCharacter(CharacterData);
        if (ParsedCharacter.bArchived) {
          CharactersStruct.Add(ParsedCharacter);
        }
      }

      FRedwoodListCharactersOutput Output;
      Output.Error = Error;
      Output.Characters = CharactersStruct;
      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::CreateCharacter(
  FString Name,
  USIOJsonObject *CharacterCreatorData,
  FRedwoodGetCharacterOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodGetCharacterOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  if (CharacterCreatorData == nullptr) {
    FRedwoodGetCharacterOutput Output;
    Output.Error = TEXT("Character creator data is required.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  Payload->SetStringField(TEXT("name"), Name);

  Payload->SetObjectField(
    TEXT("characterCreatorData"), CharacterCreatorData->GetRootObject()
  );

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:create"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetCharacterOutput Output;
      Output.Error = Error;

      const TSharedPtr<FJsonObject> *CharacterObj;
      if (MessageObject->TryGetObjectField(TEXT("character"), CharacterObj)) {
        Output.Character =
          URedwoodCommonGameSubsystem::ParseCharacter(*CharacterObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetCharacterArchived(
  FString CharacterId, bool bArchived, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        SetCharacterArchived(CharacterId, bArchived, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);
  Payload->SetBoolField(TEXT("archived"), bArchived);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:archive"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::GetCharacterData(
  FString CharacterIdOrName, FRedwoodGetCharacterOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        GetCharacterData(CharacterIdOrName, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetCharacterOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("characterIdOrName"), CharacterIdOrName);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:get"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetCharacterOutput Output;
      Output.Error = Error;

      const TSharedPtr<FJsonObject> *CharacterObj;
      if (MessageObject->TryGetObjectField(TEXT("character"), CharacterObj)) {
        Output.Character =
          URedwoodCommonGameSubsystem::ParseCharacter(*CharacterObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetCharacterData(
  FString CharacterId,
  FString Name,
  USIOJsonObject *CharacterCreatorData,
  FRedwoodGetCharacterOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See GateRealm. The strong pointer
  // keeps the data alive while the request is held.
  TStrongObjectPtr<USIOJsonObject> HeldCharacterData(CharacterCreatorData);
  if (GateRealm([=, this, Data = HeldCharacterData]() {
        SetCharacterData(CharacterId, Name, Data.Get(), OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetCharacterOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);

  if (!Name.IsEmpty()) {
    Payload->SetStringField(TEXT("name"), Name);
  }

  if (IsValid(CharacterCreatorData)) {
    Payload->SetObjectField(
      TEXT("characterCreatorData"), CharacterCreatorData->GetRootObject()
    );
  }

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:characters:set:client"),
    Payload,
    TrackReply([this, OnOutput, CharacterId](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetCharacterOutput Output;
      Output.Error = Error;

      const TSharedPtr<FJsonObject> *CharacterObj;
      if (MessageObject->TryGetObjectField(TEXT("character"), CharacterObj)) {
        Output.Character =
          URedwoodCommonGameSubsystem::ParseCharacter(*CharacterObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetSelectedCharacter(FString CharacterId) {
  // FORK(hollowed-oath): HollowedOath#2886. A new character is at character
  // select until it joins. A dropped join was the old character's; its owed
  // leave stays, because the old ticket still needs it.
  if (CharacterId != SelectedCharacterId) {
    bAssignmentExpected = false;
    bInWorld = false;
    bAbandonedQueueJoin = false;
    bTravelPending = false;
    // HollowedOath#3002. Only the join was the old character's: the ended
    // tickets stay ended, and a held notice waits for its own character.
    AcceptedPartyTicketId.Reset();
  }
  SelectedCharacterId = CharacterId;
  if (!PendingPartyTicketId.IsEmpty() && PendingPartyCharacterId == CharacterId) {
    AcceptPartyTicket(PendingPartyTicketId);
    PendingPartyTicketId.Reset();
  }

  // FORK(hollowed-oath): HollowedOath#2448. The reconnect sends what a
  // session that is not in its realm yet cannot send now.
  bSelectCharacterOwedToRealm = !IsRealmReady();
  if (!bSelectCharacterOwedToRealm) {
    SendSelectCharacter();
  }
}

// FORK(hollowed-oath): HollowedOath#2448. A party never has one member: the
// realm dissolves it and sends the last member the roster with no members.
// That leaves no party, not a valid party with no one in it. It dissolves only
// the party it names, so a late one for an earlier party keeps the held one,
// and the game must not see that party end.
bool URedwoodClientInterface::ApplyPartyChange(
  FRedwoodParty &InOutHeld, const FRedwoodParty &Changed
) {
  if (!Changed.Members.IsEmpty()) {
    InOutHeld = Changed;
    return true;
  }
  if (Changed.Id != InOutHeld.Id) {
    return false;
  }
  InOutHeld = FRedwoodParty();
  return true;
}

// FORK(hollowed-oath): HollowedOath#2448. Upstream sent this only in a party.
// A player with no party can have invites out, and the realm makes the party
// from them later, so it must know the character then too. One step: the
// realm checks the owner and selects the character for the party, then
// Bifrost records the online state at the director.
void URedwoodClientInterface::SendSelectCharacter() {
  bSelectCharacterOwedToRealm = false;

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  Connection->Step(
    TEXT("session.select-character"),
    Payload,
    [](auto Response) {
      const TSharedPtr<FJsonObject> Object = Response[0]->AsObject();
      const FString Error = Object->GetStringField(TEXT("error"));
      // A reply lost with the socket is not a refusal: the resume sends the
      // selection again when the session lacks it.
      FString Code;
      Object->TryGetStringField(TEXT("code"), Code);
      if (!Error.IsEmpty() && Code != TEXT("not_connected")) {
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT("The character selection was refused: %s"),
          *Error
        );
      }
    }
  );
}

// The server can still hold a ticket whose join reply was lost, and replay
// its assignment. Leaving deletes both. Sent at the first Realm auth after
// the loss, whatever came between; the reply to the auth is the first moment
// the Realm knows the player. The flag stays until a leave succeeds or a join
// replaces the ticket; with a join out, the leave would cancel it.
void URedwoodClientInterface::SendOwedLeave() {
  // Only this client's own join would lose its ticket to the leave; a
  // party join is the leader's ticket (HollowedOath#3002).
  if (!bLeaveTicketingOwed ||
      (bAssignmentExpected && AcceptedPartyTicketId.IsEmpty()) ||
      !IsRealmConnected()) {
    return;
  }
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Connection->Rpc(
    TEXT("realm:ticketing:leave"),
    Payload,
    Replies.Track([this](auto Response) {
      if (Response[0]->AsObject()->GetStringField(TEXT("error")).IsEmpty()) {
        bLeaveTicketingOwed = false;
      }
    }, []() {})
  );
}

// Not AuthToken: a failed re-login writes the EMPTY ids of its reply before it
// checks the error, and the requests of a player who was logged in must still
// fail then, not go out to a socket that does not know the player.
bool URedwoodClientInterface::HasPlayerSession() const {
  return bAuthenticated || bLoggedInAtDrop;
}

// FORK(hollowed-oath): a request rides the session channel, so with no
// session on the connection (before the login, after a logout, after Bifrost
// ended the session) it cannot be sent, even while the socket is still open.
bool URedwoodClientInterface::CanSendToDirector() {
  return FRedwoodHeldRequests::CanSend(
    IsDirectorConnected() && Connection->HasSession(),
    bAuthenticated && !bReauthPending,
    HasPlayerSession()
  );
}

bool URedwoodClientInterface::CanSendToRealm() {
  return FRedwoodHeldRequests::CanSend(
    IsRealmConnected(), bAuthenticated && !bReauthPending, HasPlayerSession()
  );
}

// The session is back (or not): send what the player asked for while the
// socket was away, or fail it now instead of at the end of the grace.
void URedwoodClientInterface::EndReauthentication(bool bSucceeded) {
  bReauthPending = false;

  // The player logged out while the re-login was in flight; its reply has
  // just logged them in again, so log out again.
  if (bLoggedOutDuringRelogin) {
    if (bSucceeded) {
      Logout();
    }
    return;
  }

  if (bSucceeded) {
    // bAuthenticated carries the session again.
    bAuthenticated = true;
    bLoggedInAtDrop = false;
    bDisconnected = false;
    HeldRequests.Release(TimerManager);
    OnConnectionReestablished.Broadcast();
  } else {
    HeldRequests.Expire(TimerManager);
    bRealmEntered = false;
  }
}

void URedwoodClientInterface::JoinMatchmaking(
  FString ProfileId,
  TArray<FString> InRegions,
  FRedwoodTicketingUpdateDelegate OnUpdate
) {
  if (SelectedCharacterId.IsEmpty()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message =
      TEXT("Please select a character before joining matchmaking.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TicketingProfileId = ProfileId;
  TicketingRegions = InRegions;
  OnTicketingUpdate = OnUpdate;
  AttemptJoinMatchmaking();
}

void URedwoodClientInterface::JoinQueue(
  FString ProxyId,
  FString ZoneName,
  bool bTransferWholeParty,
  bool bFavorLastZone,
  FRedwoodTicketingUpdateDelegate OnUpdate
) {
  if (SelectedCharacterId.IsEmpty()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message =
      TEXT("Please select a character before joining a server queue.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message = TEXT("Not connected to Realm.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  TSharedPtr<FJsonObject> QueueData = MakeShareable(new FJsonObject);

  QueueData->SetStringField(TEXT("proxyId"), ProxyId);
  QueueData->SetStringField(TEXT("zoneName"), ZoneName);
  QueueData->SetBoolField(TEXT("transferWholeParty"), bTransferWholeParty);
  QueueData->SetBoolField(TEXT("favorLastZone"), bFavorLastZone);

  TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
  QueueData->SetField(TEXT("priorZoneName"), NullValue);
  QueueData->SetField(TEXT("shardName"), NullValue);

  Payload->SetObjectField(TEXT("data"), QueueData);

  OnTicketingUpdate = OnUpdate;

  NoteJoinSent(); // FORK(hollowed-oath): HollowedOath#2886.
  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:ticketing:join:queue"),
    Payload,
    Replies.Track([this, Sequence = JoinSequence](auto Response) {
      HandleTicketingJoinReply(
        Response[0]->AsObject()->GetStringField(TEXT("error")), Sequence
      );
    }, [this, Sequence = JoinSequence]() { FailTicketingJoin(Sequence); })
  );
}

void URedwoodClientInterface::JoinCustom(
  bool bTransferWholeParty,
  TArray<FString> InRegions,
  FRedwoodTicketingUpdateDelegate OnUpdate
) {
  if (SelectedCharacterId.IsEmpty()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message = TEXT("Please select a character before joining.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TicketingRegions = InRegions;
  OnTicketingUpdate = OnUpdate;
  bTicketingTransferWholeParty = bTransferWholeParty;
  AttemptJoinCustom();
}

void URedwoodClientInterface::AttemptJoinCustom() {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message = TEXT("Not connected to Realm.");
    OnTicketingUpdate.ExecuteIfBound(Update);
    OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
    return;
  }

  if (SelectedCharacterId.IsEmpty()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message = TEXT("Please select a character before joining.");
    OnTicketingUpdate.ExecuteIfBound(Update);
    return;
  }

  if (PingAverages.Num() != Regions.Num()) {
    // we haven't finished getting a single set of ping averages yet
    // let's delay sending our join request
    UE_LOG(
      LogRedwood,
      Log,
      TEXT(
        "Not enough ping averages. Num averages: %d, num Regions: %d. Will attempt again."
      ),
      PingAverages.Num(),
      Regions.Num()
    );

    TimerManager.SetTimer(
      PingTimer, this, &URedwoodClientInterface::AttemptJoinCustom, 2.f, false
    );
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetBoolField(
    TEXT("transferWholeParty"), bTicketingTransferWholeParty
  );

  TArray<TSharedPtr<FJsonValue>> DesiredRegions;
  for (FString RegionName : TicketingRegions) {
    float *Ping = PingAverages.Find(RegionName);

    if (Ping == nullptr) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Could not find ping for region %s."),
        *RegionName
      );
      continue;
    }

    TSharedPtr<FJsonObject> RegionObject = MakeShareable(new FJsonObject);
    RegionObject->SetStringField(TEXT("name"), RegionName);
    RegionObject->SetNumberField(TEXT("ping"), *Ping);

    TSharedPtr<FJsonValueObject> Value =
      MakeShareable(new FJsonValueObject(RegionObject));

    DesiredRegions.Add(Value);
  }
  Payload->SetArrayField(TEXT("regions"), DesiredRegions);

  NoteJoinSent(); // FORK(hollowed-oath): HollowedOath#2886.
  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:ticketing:join:custom"),
    Payload,
    Replies.Track([this, Sequence = JoinSequence](auto Response) {
      HandleTicketingJoinReply(
        Response[0]->AsObject()->GetStringField(TEXT("error")), Sequence
      );
    }, [this, Sequence = JoinSequence]() { FailTicketingJoin(Sequence); })
  );
}

void URedwoodClientInterface::LeaveTicketing(
  FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. The game leaves when it gives up
  // on a join, often because the Realm dropped. Whatever the server does with
  // the leave, the join's assignment must not move the player, and a leave
  // that cannot go out now is owed.
  bAbandonedQueueJoin = true;
  bAssignmentExpected = false;

  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    bLeaveTicketingOwed = true;
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:ticketing:leave"),
    Payload,
    Replies.Track([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        bLeaveTicketingOwed = false;
      }
      OnOutput.ExecuteIfBound(Error);
    }, [this, OnLost = MakeLostReply(OnOutput)]() {
      // A leave that may not have run is owed.
      bLeaveTicketingOwed = true;
      OnLost();
    })
  );
}

void URedwoodClientInterface::ListProxies(
  TArray<FString> PrivateProxyReferences,
  FRedwoodListProxiesOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodListProxiesOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  TArray<TSharedPtr<FJsonValue>> PrivateProxyReferencesArray;
  for (FString Reference : PrivateProxyReferences) {
    TSharedPtr<FJsonValueString> Value =
      MakeShareable(new FJsonValueString(Reference));
    PrivateProxyReferencesArray.Add(Value);
  }
  Payload->SetArrayField(
    TEXT("privateProxyReferences"), PrivateProxyReferencesArray
  );

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:servers:list-proxies"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      TArray<TSharedPtr<FJsonValue>> Proxies =
        MessageObject->GetArrayField(TEXT("proxies"));

      TArray<FRedwoodGameServerProxy> ProxiesStruct;
      for (TSharedPtr<FJsonValue> Proxy : Proxies) {
        TSharedPtr<FJsonObject> ProxyData = Proxy->AsObject();
        ProxiesStruct.Add(
          URedwoodCommonGameSubsystem::ParseServerProxy(ProxyData)
        );
      }

      FRedwoodListProxiesOutput Output;
      Output.Error = Error;
      Output.Proxies = ProxiesStruct;
      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::CreateProxy(
  bool bJoinSession,
  FRedwoodCreateProxyInput Parameters,
  FRedwoodCreateProxyOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodCreateProxyOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  if (bJoinSession) {
    if (SelectedCharacterId.IsEmpty()) {
      FRedwoodCreateProxyOutput Output;
      Output.Error =
        TEXT("Please select a character before joining a session.");
      OnOutput.ExecuteIfBound(Output);
      return;
    }

    Payload->SetStringField(TEXT("joinCharacterId"), SelectedCharacterId);
  } else {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("joinCharacterId"), NullValue);
  }

  Payload->SetStringField(TEXT("name"), Parameters.Name);

  Payload->SetStringField(TEXT("region"), Parameters.Region);

  Payload->SetStringField(TEXT("modeId"), Parameters.ModeId);

  if (!Parameters.MapId.IsEmpty()) {
    Payload->SetStringField(TEXT("mapId"), Parameters.MapId);
  } else {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("mapId"), NullValue);
  }

  Payload->SetBoolField(TEXT("public"), Parameters.bPublic);

  Payload->SetBoolField(
    TEXT("proxyEndsWhenCollectionEnds"), Parameters.bProxyEndsWhenCollectionEnds
  );

  Payload->SetBoolField(TEXT("continuousPlay"), Parameters.bContinuousPlay);

  if (!Parameters.Password.IsEmpty()) {
    Payload->SetStringField(TEXT("password"), Parameters.Password);
  } else {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("password"), NullValue);
  }

  if (!Parameters.ShortCode.IsEmpty()) {
    Payload->SetStringField(TEXT("shortCode"), Parameters.ShortCode);
  } else {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("shortCode"), NullValue);
  }

  if (Parameters.Data) {
    Payload->SetObjectField(TEXT("data"), Parameters.Data->GetRootObject());
  }

  Payload->SetBoolField(TEXT("startOnBoot"), Parameters.bStartOnBoot);

  // FORK(hollowed-oath): HollowedOath#2886. A create that joins its session
  // gets its assignment as a connect-to-instance, like a ticketing join.
  if (bJoinSession) {
    NoteJoinSent();
  }
  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:servers:create-proxy"),
    Payload,
    Replies.Track([this, OnOutput, bJoinSession, Sequence = JoinSequence](
      auto Response
    ) {
      FRedwoodCreateProxyOutput Output;

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      Output.Error = MessageObject->GetStringField(TEXT("error"));
      if (bJoinSession) {
        NoteJoinAnswered(Output.Error, Sequence); // FORK(hollowed-oath)
      }

      MessageObject->TryGetStringField(
        TEXT("proxyReference"), Output.ProxyReference
      );

      OnOutput.ExecuteIfBound(Output);
    }, bJoinSession ? MakeLostProxyJoin(JoinSequence, MakeLostReply(OnOutput))
                    : MakeLostReply(OnOutput))
  );
}

void URedwoodClientInterface::JoinProxyWithSingleInstance(
  FString ProxyReference,
  FString Password,
  FRedwoodJoinServerOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodJoinServerOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  if (SelectedCharacterId.IsEmpty()) {
    FRedwoodJoinServerOutput Output;
    Output.Error = TEXT("Please select a character before joining a session.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  Payload->SetStringField(TEXT("proxyReference"), ProxyReference);

  if (!Password.IsEmpty()) {
    Payload->SetStringField(TEXT("password"), Password);
  } else {
    TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());
    Payload->SetField(TEXT("password"), NullValue);
  }

  NoteJoinSent(); // FORK(hollowed-oath): HollowedOath#2886.
  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:servers:join-proxy"),
    Payload,
    Replies.Track([this, OnOutput, Sequence = JoinSequence](auto Response) {
      FRedwoodJoinServerOutput Output;

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      Output.Error = MessageObject->GetStringField(TEXT("error"));
      NoteJoinAnswered(Output.Error, Sequence); // FORK(hollowed-oath)

      MessageObject->TryGetStringField(
        TEXT("connectionUri"), Output.ConnectionUri
      );
      MessageObject->TryGetStringField(TEXT("token"), Output.Token);

      OnOutput.ExecuteIfBound(Output);
    }, MakeLostProxyJoin(JoinSequence, MakeLostReply(OnOutput)))
  );
}

void URedwoodClientInterface::StopProxy(
  FString ServerProxyId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);

  Payload->SetStringField(TEXT("proxyId"), ServerProxyId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:servers:stop-proxy"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      OnOutput.ExecuteIfBound(MessageObject->GetStringField(TEXT("error")));
    }, OnOutput)
  );
}

void URedwoodClientInterface::AttemptJoinMatchmaking() {
  // FORK(hollowed-oath): HollowedOath#2886. See IsRealmReady.
  if (!IsRealmReady()) {
    FRedwoodTicketingUpdate Update;
    Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
    Update.Message = TEXT("Not connected to Realm.");
    OnTicketingUpdate.ExecuteIfBound(Update);
    OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
    return;
  }

  if (PingAverages.Num() != Regions.Num()) {
    // we haven't finished getting a single set of ping averages yet
    // let's delay sending our join request
    UE_LOG(
      LogRedwood,
      Log,
      TEXT(
        "Not enough ping averages. Num averages: %d, num Regions: %d. Will attempt again."
      ),
      PingAverages.Num(),
      Regions.Num()
    );

    TimerManager.SetTimer(
      PingTimer,
      this,
      &URedwoodClientInterface::AttemptJoinMatchmaking,
      2.f,
      false
    );
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  TSharedPtr<FJsonObject> MatchmakingData = MakeShareable(new FJsonObject);

  MatchmakingData->SetStringField(TEXT("profileId"), TicketingProfileId);

  TArray<TSharedPtr<FJsonValue>> DesiredRegions;
  for (FString RegionName : TicketingRegions) {
    float *Ping = PingAverages.Find(RegionName);

    if (Ping == nullptr) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Could not find ping for region %s."),
        *RegionName
      );
      continue;
    }

    TSharedPtr<FJsonObject> RegionObject = MakeShareable(new FJsonObject);
    RegionObject->SetStringField(TEXT("name"), RegionName);
    RegionObject->SetNumberField(TEXT("ping"), *Ping);

    TSharedPtr<FJsonValueObject> Value =
      MakeShareable(new FJsonValueObject(RegionObject));

    DesiredRegions.Add(Value);
  }
  MatchmakingData->SetArrayField(TEXT("regions"), DesiredRegions);

  Payload->SetObjectField(TEXT("data"), MatchmakingData);

  NoteJoinSent(); // FORK(hollowed-oath): HollowedOath#2886.
  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:ticketing:join:matchmaking"),
    Payload,
    Replies.Track([this, Sequence = JoinSequence](auto Response) {
      HandleTicketingJoinReply(
        Response[0]->AsObject()->GetStringField(TEXT("error")), Sequence
      );
    }, [this, Sequence = JoinSequence]() { FailTicketingJoin(Sequence); })
  );
}

void URedwoodClientInterface::GetOrCreateParty(
  bool bCreateIfNotInParty, FRedwoodGetPartyOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        GetOrCreateParty(bCreateIfNotInParty, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetPartyOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  if (SelectedCharacterId == TEXT("")) {
    FRedwoodGetPartyOutput Output;
    Output.Error = TEXT("Please select a character before joining a party.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetBoolField(TEXT("createIfNotInParty"), bCreateIfNotInParty);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:get:frontend"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetPartyOutput Output;
      Output.Error = Error;

      if (Error.IsEmpty()) {
        const TSharedPtr<FJsonObject> *PartyObj;
        if (MessageObject->TryGetObjectField(TEXT("party"), PartyObj)) {
          Output.Party = URedwoodCommonGameSubsystem::ParseParty(*PartyObj);
          CurrentParty = Output.Party;
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::LeaveParty(FRedwoodErrorOutputDelegate OnOutput) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        LeaveParty(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:leave"),
    Payload,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        CurrentParty = FRedwoodParty();
      }

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::InviteToParty(
  FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2448. The invite names the inviter's
  // character, so a held invite must not go out for one selected after it.
  const FString InviterCharacterId = SelectedCharacterId;
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        if (InviterCharacterId != SelectedCharacterId) {
          OnOutput.ExecuteIfBound(
            URedwoodCommonGameSubsystem::CharacterChangedError
          );
          return;
        }
        InviteToParty(TargetPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);
  // FORK(hollowed-oath): HollowedOath#2448. The realm makes a party only when
  // an invite is accepted, so it needs the inviter's character from the invite.
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:invites:initiate"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListPartyInvites(
  FRedwoodListPartyInvitesOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        ListPartyInvites(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodListPartyInvitesOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:invites:get"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodListPartyInvitesOutput Output;
      Output.Error = Error;
      if (Error.IsEmpty()) {
        TArray<TSharedPtr<FJsonValue>> Invites =
          MessageObject->GetArrayField(TEXT("invites"));

        Output.Invites =
          URedwoodCommonGameSubsystem::ParsePartyInvites(Invites);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::RespondToPartyInvite(
  FString PartyId, bool bAccept, FRedwoodGetPartyOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        RespondToPartyInvite(PartyId, bAccept, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetPartyOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  if (SelectedCharacterId == TEXT("")) {
    FRedwoodGetPartyOutput Output;
    Output.Error =
      TEXT("Please select a character before responding to a party invite.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetStringField(TEXT("partyId"), PartyId);
  Payload->SetBoolField(TEXT("accept"), bAccept);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:invites:respond"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetPartyOutput Output;
      Output.Error = Error;

      if (Error.IsEmpty()) {
        const TSharedPtr<FJsonObject> *PartyObj;
        if (MessageObject->TryGetObjectField(TEXT("party"), PartyObj)) {
          Output.Party = URedwoodCommonGameSubsystem::ParseParty(*PartyObj);
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::PromoteToPartyLeader(
  FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        PromoteToPartyLeader(TargetPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:promote"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::KickFromParty(
  FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        KickFromParty(TargetPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:kick"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SetPartyData(
  FString LootType,
  USIOJsonObject *PartyData,
  FRedwoodGetPartyOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2886. See GateRealm. The strong pointer
  // keeps the data alive while the request is held.
  TStrongObjectPtr<USIOJsonObject> HeldPartyData(PartyData);
  if (GateRealm([=, this, Data = HeldPartyData]() {
        SetPartyData(LootType, Data.Get(), OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetPartyOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  if (!LootType.IsEmpty()) {
    Payload->SetStringField(TEXT("lootType"), LootType);
  }

  if (IsValid(PartyData)) {
    Payload->SetObjectField(TEXT("data"), PartyData->GetRootObject());
  }

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:parties:set"),
    Payload,
    TrackReply([OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetPartyOutput Output;
      Output.Error = Error;

      if (Error.IsEmpty()) {
        const TSharedPtr<FJsonObject> *PartyObj;
        if (MessageObject->TryGetObjectField(TEXT("party"), PartyObj)) {
          Output.Party = URedwoodCommonGameSubsystem::ParseParty(*PartyObj);
        }
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::SendEmoteToParty(FString Emote) {
  if (!IsRealmConnected() || !CurrentParty.bValid) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "Cannot send emote to party: not connected to Realm or not in a party."
      )
    );
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("emote"), Emote);

  Connection->Rpc(TEXT("realm:parties:emote"), Payload);
}

FString URedwoodClientInterface::GetConnectionConsoleCommand() {
  if (ServerConnection.IsEmpty() || ServerToken.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Server connection or token is empty."));
    return "";
  }

  if (SelectedCharacterId.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Selected character ID is empty."));
    return "";
  }

  TMap<FString, FString> Options;
  Options.Add("RedwoodAuth", "1");
  Options.Add("CharacterId", SelectedCharacterId);
  Options.Add("PlayerId", PlayerId);
  Options.Add("Token", ServerToken);

  TArray<FString> JoinedOptions;
  for (const TPair<FString, FString> &Option : Options) {
    JoinedOptions.Add(Option.Key + "=" + Option.Value);
  }

  FString OptionsString =
    UKismetStringLibrary::JoinStringArray(JoinedOptions, "?");
  FString ConnectionString =
    TEXT("open ") + ServerConnection + "?" + OptionsString;

  return ConnectionString;
}

FURL URedwoodClientInterface::GetConnectionURL() {
  FURL URL;
  URL.Valid = 0;
  if (ServerConnection.IsEmpty() || ServerToken.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Server connection or token is empty."));
    return URL;
  }

  if (SelectedCharacterId.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Selected character ID is empty."));
    return URL;
  }

  FString Host;
  FString Port;
  ServerConnection.Split(":", &Host, &Port);

  URL.Protocol = TEXT("unreal");
  URL.Host = Host;
  URL.Port = FCString::Atoi(*Port);
  URL.Valid = 1;

  TMap<FString, FString> Options;
  Options.Add("RedwoodAuth", "1");
  Options.Add("CharacterId", SelectedCharacterId);
  Options.Add("PlayerId", PlayerId);
  Options.Add("Token", ServerToken);
  for (const TPair<FString, FString> &Option : Options) {
    URL.AddOption(*(Option.Key + "=" + Option.Value));
  }

  return URL;
}

void URedwoodClientInterface::GetDirectorGlobalData(
  FRedwoodGetGlobalDataOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        GetDirectorGlobalData(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsDirectorConnected()) {
    FRedwoodGetGlobalDataOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("director:global-data:get:latest"),
    nullptr,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetGlobalDataOutput Output;

      if (!Error.IsEmpty()) {
        Output.Error = Error;
        OnOutput.ExecuteIfBound(Output);
        return;
      }

      Output.Id = MessageObject->GetNumberField(TEXT("id"));

      const TSharedPtr<FJsonObject> *DataObj;
      if (MessageObject->TryGetObjectField(TEXT("data"), DataObj)) {
        Output.Data = NewObject<USIOJsonObject>();
        Output.Data->SetRootObject(*DataObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}

void URedwoodClientInterface::GetRealmGlobalData(
  FRedwoodGetGlobalDataOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        GetRealmGlobalData(OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!IsRealmConnected()) {
    FRedwoodGetGlobalDataOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Connection->Rpc(
    TEXT("realm:global-data:get:latest"),
    nullptr,
    TrackReply([this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodGetGlobalDataOutput Output;

      if (!Error.IsEmpty()) {
        Output.Error = Error;
        OnOutput.ExecuteIfBound(Output);
        return;
      }

      Output.Id = MessageObject->GetNumberField(TEXT("id"));

      const TSharedPtr<FJsonObject> *DataObj;
      if (MessageObject->TryGetObjectField(TEXT("data"), DataObj)) {
        Output.Data = NewObject<USIOJsonObject>();
        Output.Data->SetRootObject(*DataObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
  );
}
