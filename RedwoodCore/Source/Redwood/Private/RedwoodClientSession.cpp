// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodClientSession.h"
#include "RedwoodModule.h"
#include "RedwoodPendingReplies.h"

const TCHAR *const FRedwoodClientSession::NotConnectedToDirector =
  TEXT("Not connected to Director.");
const TCHAR *const FRedwoodClientSession::NotConnectedToRealm =
  TEXT("Not connected to Realm.");
const TCHAR *const FRedwoodClientSession::LoggedOutError = TEXT("Logged out.");
const TCHAR *const FRedwoodClientSession::ReplacedMessage =
  TEXT("This account logged in from another client.");
const TCHAR *const FRedwoodClientSession::NewerLoginError =
  TEXT("A newer login replaced this one.");
const TCHAR *const FRedwoodClientSession::OtherRealmError =
  TEXT("The session is in another realm; log in again.");
const TCHAR *const FRedwoodClientSession::EntryInProgressError =
  TEXT("A realm entry is in progress.");
const TCHAR *const FRedwoodClientSession::EntryNotReadyError =
  TEXT("Not connected or authenticated to Director.");

namespace {
  const TCHAR *const MustVerifyMessage = TEXT("Must verify account");
  const TCHAR *const LocalProvider = TEXT("local");
}

// ── Events ──────────────────────────────────────────────────────────────

FRedwoodSessionEvent FRedwoodSessionEvent::SocketUp() {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::SocketUp;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::SocketDown(bool bAnnounced) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::SocketDown;
  Event.bAnnounced = bAnnounced;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::ResumeAccepted(
  const FString &RealmId,
  const FString &LockedRealmId,
  const FString &CharacterId
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::ResumeAccepted;
  Event.RealmId = RealmId;
  Event.LockedRealmId = LockedRealmId;
  Event.CharacterId = CharacterId;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::ResumeRefused() {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::ResumeRefused;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::ClosedByServer(
  const FString &Reason
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::ClosedByServer;
  Event.Reason = Reason;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::LoginOk(
  uint32 Seq,
  const FString &PlayerId,
  const FString &Token,
  const FString &Nickname
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::LoginReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Ok;
  Event.PlayerId = PlayerId;
  Event.Token = Token;
  Event.Nickname = Nickname;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::LoginRefused(
  uint32 Seq, const FString &Reason, const FString &Message
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::LoginReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Refused;
  Event.Reason = Reason;
  Event.Message = Message;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::LoginLost(uint32 Seq) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::LoginReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Lost;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::EnterRealmOk(
  uint32 Seq, const TSharedPtr<FJsonObject> &Response
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::EnterRealmReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Ok;
  Event.Response = Response;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::EnterRealmRefused(
  uint32 Seq, const FString &Error
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::EnterRealmReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Refused;
  Event.Message = Error;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::EnterRealmLost(uint32 Seq) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::EnterRealmReply;
  Event.Seq = Seq;
  Event.Outcome = ERedwoodReplyOutcome::Lost;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::GraceExpired() {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::GraceExpired;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::Initialize() {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::Initialize;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::Login(
  uint32 RequestId,
  const FString &Username,
  const FString &Secret,
  const FString &Provider,
  bool bRememberMe
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::Login;
  Event.RequestId = RequestId;
  Event.Username = Username;
  Event.Secret = Secret;
  Event.Provider = Provider;
  Event.bRememberMe = bRememberMe;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::Logout() {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::Logout;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::EnterRealm(
  uint32 RequestId, const FString &RealmId
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::EnterRealm;
  Event.RequestId = RequestId;
  Event.RealmId = RealmId;
  return Event;
}

FRedwoodSessionEvent FRedwoodSessionEvent::SelectCharacter(
  const FString &CharacterId
) {
  FRedwoodSessionEvent Event;
  Event.Kind = ERedwoodSessionEventKind::SelectCharacter;
  Event.CharacterId = CharacterId;
  return Event;
}

// ── Effects ─────────────────────────────────────────────────────────────

bool FRedwoodSessionEffect::operator==(
  const FRedwoodSessionEffect &Other
) const {
  return Kind == Other.Kind && Seq == Other.Seq &&
    RequestId == Other.RequestId && LoginAnswer == Other.LoginAnswer &&
    Text == Other.Text && Username == Other.Username &&
    Secret == Other.Secret && Provider == Other.Provider;
}

FString FRedwoodSessionEffect::ToString() const {
  FString Result = FRedwoodClientSession::EffectName(Kind);
  if (Seq != 0) {
    Result += FString::Printf(TEXT(" seq=%u"), Seq);
  }
  if (RequestId != 0) {
    Result += FString::Printf(TEXT(" req=%u"), RequestId);
  }
  if (Kind == ERedwoodSessionEffectKind::AnswerLogin) {
    Result += LoginAnswer == ERedwoodLoginAnswer::Success ? TEXT(" success")
      : LoginAnswer == ERedwoodLoginAnswer::MustVerify    ? TEXT(" must-verify")
                                                          : TEXT(" error");
  }
  if (!Text.IsEmpty()) {
    Result += TEXT(" \"") + Text + TEXT("\"");
  }
  if (!Username.IsEmpty()) {
    Result += TEXT(" user=") + Username;
  }
  if (!Provider.IsEmpty()) {
    Result += TEXT(" provider=") + Provider;
  }
  return Result;
}

// ── Queries ─────────────────────────────────────────────────────────────

bool FRedwoodClientSession::IsOutagePhase(ERedwoodSessionPhase InPhase) {
  switch (InPhase) {
    case ERedwoodSessionPhase::ConnectFailed:
    case ERedwoodSessionPhase::Offline:
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Relogging:
    case ERedwoodSessionPhase::Reentering:
      return true;
    default:
      return false;
  }
}

bool FRedwoodClientSession::IsComingBackPhase(ERedwoodSessionPhase InPhase) {
  switch (InPhase) {
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Relogging:
    case ERedwoodSessionPhase::Reentering:
      return true;
    default:
      return false;
  }
}

bool FRedwoodClientSession::IsLoggedInPhase(ERedwoodSessionPhase InPhase) {
  return InPhase == ERedwoodSessionPhase::LoggedIn ||
    InPhase == ERedwoodSessionPhase::EnteringRealm ||
    InPhase == ERedwoodSessionPhase::InRealm;
}

bool FRedwoodClientSession::IsSocketUpPhase(ERedwoodSessionPhase InPhase) {
  switch (InPhase) {
    case ERedwoodSessionPhase::Connecting:
    case ERedwoodSessionPhase::ConnectFailed:
    case ERedwoodSessionPhase::Offline:
    case ERedwoodSessionPhase::OfflineQuiet:
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
      return false;
    default:
      return true;
  }
}

bool FRedwoodClientSession::IsSocketUp() const {
  return IsSocketUpPhase(Phase);
}

bool FRedwoodClientSession::IsLoggedIn() const {
  return IsLoggedInPhase(Phase);
}

bool FRedwoodClientSession::HasPlayerSession() const {
  return IsLoggedInPhase(Phase) || IsComingBackPhase(Phase);
}

bool FRedwoodClientSession::IsRealmEntered() const {
  return Phase == ERedwoodSessionPhase::InRealm;
}

bool FRedwoodClientSession::IsOutage() const {
  return IsOutagePhase(Phase);
}

bool FRedwoodClientSession::IsComingBack() const {
  return IsComingBackPhase(Phase);
}

FString FRedwoodClientSession::GetRealmId() const {
  return Phase == ERedwoodSessionPhase::EnteringRealm ? FString() : RealmId;
}

// A request is sent when the session is live and, for the realm, in the
// realm; held while the session is coming back, unless its grace ran out;
// failed at once in every other phase. A realm request is held only when
// there is a realm to come back to.
ERedwoodGateDecision FRedwoodClientSession::Gate(
  ERedwoodRequestKind Kind
) const {
  switch (Phase) {
    case ERedwoodSessionPhase::LoggedIn:
    case ERedwoodSessionPhase::EnteringRealm:
      return Kind == ERedwoodRequestKind::Director ? ERedwoodGateDecision::Send
                                                   : ERedwoodGateDecision::Fail;
    case ERedwoodSessionPhase::InRealm:
      return ERedwoodGateDecision::Send;
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Relogging:
    case ERedwoodSessionPhase::Reentering:
      if (bGraceExpired) {
        return ERedwoodGateDecision::Fail;
      }
      if (Kind == ERedwoodRequestKind::Realm && RealmId.IsEmpty()) {
        return ERedwoodGateDecision::Fail;
      }
      return ERedwoodGateDecision::Hold;
    default:
      return ERedwoodGateDecision::Fail;
  }
}

// ── Handle ──────────────────────────────────────────────────────────────

TArray<FRedwoodSessionEffect> FRedwoodClientSession::Handle(
  const FRedwoodSessionEvent &Event
) {
  Out.Reset();
  const bool bWasOutage = IsOutage();

  switch (Event.Kind) {
    case ERedwoodSessionEventKind::SocketUp:
      OnSocketUp();
      break;
    case ERedwoodSessionEventKind::SocketDown:
      OnSocketDown(Event);
      break;
    case ERedwoodSessionEventKind::ResumeAccepted:
      OnResumeAccepted(Event);
      break;
    case ERedwoodSessionEventKind::ResumeRefused:
      OnResumeRefused();
      break;
    case ERedwoodSessionEventKind::ClosedByServer:
      OnClosedByServer(Event);
      break;
    case ERedwoodSessionEventKind::LoginReply:
      OnLoginReply(Event);
      break;
    case ERedwoodSessionEventKind::EnterRealmReply:
      OnEnterRealmReply(Event);
      break;
    case ERedwoodSessionEventKind::GraceExpired:
      OnGraceExpired();
      break;
    case ERedwoodSessionEventKind::Initialize:
      OnInitialize();
      break;
    case ERedwoodSessionEventKind::Login:
      OnLogin(Event);
      break;
    case ERedwoodSessionEventKind::Logout:
      OnLogout();
      break;
    case ERedwoodSessionEventKind::EnterRealm:
      OnEnterRealm(Event);
      break;
    case ERedwoodSessionEventKind::SelectCharacter:
      OnSelectCharacter(Event);
      break;
    default:
      break;
  }

  // The one place a lost connection is broadcast: the phase entered an
  // outage. A predicate flips to true at most once before it flips back, so
  // one outage is one broadcast however many sockets fail inside it.
  if (!bWasOutage && IsOutage()) {
    Emit(ERedwoodSessionEffectKind::BroadcastLost);
  }

  TArray<FRedwoodSessionEffect> Result = MoveTemp(Out);
  Out.Reset();
  return Result;
}

// ── Rows ────────────────────────────────────────────────────────────────

void FRedwoodClientSession::OnSocketUp() {
  switch (Phase) {
    case ERedwoodSessionPhase::Connecting:
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::ReportFirstConnect);
      return;
    case ERedwoodSessionPhase::ConnectFailed:
      // The loss of a first connect that failed is over when it connects;
      // the game's message for it comes down here.
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::ReportFirstConnect);
      Emit(ERedwoodSessionEffectKind::BroadcastReestablished);
      return;
    case ERedwoodSessionPhase::Offline:
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::BroadcastReestablished);
      return;
    case ERedwoodSessionPhase::OfflineQuiet:
      Go(ERedwoodSessionPhase::Online);
      return;
    case ERedwoodSessionPhase::Suspended:
      // The connection resumes the session by itself on the new socket.
      Go(ERedwoodSessionPhase::Resuming);
      return;
    case ERedwoodSessionPhase::ReloginPending:
      Go(ERedwoodSessionPhase::Relogging);
      EmitSendLogin(PlayerId, AuthToken, LocalProvider);
      return;
    default:
      // A socket the machine never saw go down.
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT("The socket came up in phase %s, which has one already."),
        PhaseName(Phase)
      );
      return;
  }
}

void FRedwoodClientSession::OnSocketDown(const FRedwoodSessionEvent &Event) {
  switch (Phase) {
    case ERedwoodSessionPhase::Connecting:
      Go(ERedwoodSessionPhase::ConnectFailed);
      Emit(ERedwoodSessionEffectKind::LogFirstConnectFailure);
      return;
    case ERedwoodSessionPhase::Online:
      // Bifrost closes the socket itself only after `closed`, when the
      // session is over and the game heard it: that close is no loss.
      Go(
        Event.bAnnounced ? ERedwoodSessionPhase::Offline
                         : ERedwoodSessionPhase::OfflineQuiet
      );
      Emit(ERedwoodSessionEffectKind::FailPendingReplies);
      return;
    case ERedwoodSessionPhase::LoggingIn:
      // Normally the lost join reply came first and left LoggingIn already.
      Go(ERedwoodSessionPhase::Offline);
      Emit(ERedwoodSessionEffectKind::FailPendingReplies);
      EmitAnswerLogin(
        PendingLoginRequest,
        ERedwoodLoginAnswer::Error,
        FRedwoodPendingReplies::LostReplyError
      );
      PendingLoginRequest = 0;
      return;
    case ERedwoodSessionPhase::LoggedIn:
      Suspend(FString());
      return;
    case ERedwoodSessionPhase::EnteringRealm:
      // The realm may or may not have started the session; the resume's
      // reply says which realm, if any, the session is locked to.
      Suspend(FString());
      AnswerPendingEntries(FRedwoodPendingReplies::LostReplyError);
      return;
    case ERedwoodSessionPhase::InRealm:
      Suspend(RealmId);
      return;
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Reentering:
      // The same outage: no second loss, the grace runs on.
      Go(ERedwoodSessionPhase::Suspended);
      return;
    case ERedwoodSessionPhase::Relogging:
      Go(ERedwoodSessionPhase::ReloginPending);
      return;
    case ERedwoodSessionPhase::LoggingOut:
      Go(ERedwoodSessionPhase::Offline);
      return;
    default:
      // ConnectFailed, Offline, OfflineQuiet, Suspended, ReloginPending: the
      // socket was down already.
      Ignore(Event);
      return;
  }
}

void FRedwoodClientSession::OnResumeAccepted(
  const FRedwoodSessionEvent &Event
) {
  switch (Phase) {
    case ERedwoodSessionPhase::Resuming: {
      if (!Event.RealmId.IsEmpty()) {
        RealmId = Event.RealmId;
        // The realm selects the character the session names; one this
        // client selected since is sent again.
        if (
          !SelectedCharacterId.IsEmpty() &&
          Event.CharacterId != SelectedCharacterId
        ) {
          EmitText(
            ERedwoodSessionEffectKind::SendSelectCharacter, SelectedCharacterId
          );
        }
        Restore(ERedwoodSessionPhase::InRealm);
        return;
      }
      // The realm did not answer the start before the drop. The session is
      // locked to one realm and no other is accepted: the one this client
      // asked for, or the one Bifrost names when this client's own entry
      // was lost with the socket before it learned the realm.
      const FString Target = !RealmId.IsEmpty() ? RealmId : Event.LockedRealmId;
      if (!Target.IsEmpty()) {
        RealmId = Target;
        Go(ERedwoodSessionPhase::Reentering);
        EmitSendEnterRealm(Target);
        return;
      }
      Restore(ERedwoodSessionPhase::LoggedIn);
      return;
    }
    case ERedwoodSessionPhase::LoggingOut:
      // The player logged out while this resume was in flight; the join
      // brought back a session that must end now.
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      Emit(ERedwoodSessionEffectKind::ForgetSession);
      return;
    default:
      Ignore(Event);
      return;
  }
}

void FRedwoodClientSession::OnResumeRefused() {
  switch (Phase) {
    case ERedwoodSessionPhase::Resuming:
      // The session is gone; the token logs the player in again.
      Go(ERedwoodSessionPhase::Relogging);
      EmitSendLogin(PlayerId, AuthToken, LocalProvider);
      return;
    case ERedwoodSessionPhase::LoggingOut:
      // Nothing came back, and the player left: nothing to log in with.
      Go(ERedwoodSessionPhase::Online);
      return;
    default:
      Ignore(FRedwoodSessionEvent::ResumeRefused());
      return;
  }
}

void FRedwoodClientSession::OnClosedByServer(
  const FRedwoodSessionEvent &Event
) {
  // `reconnect` and `logout` carry nothing the socket's end does not: the
  // SocketDown (or the quiet channel end) that follows says it all.
  if (Event.Reason != TEXT("replaced")) {
    return;
  }
  switch (Phase) {
    case ERedwoodSessionPhase::LoggedIn:
    case ERedwoodSessionPhase::InRealm:
    case ERedwoodSessionPhase::Reentering:
    case ERedwoodSessionPhase::EnteringRealm:
      Go(ERedwoodSessionPhase::Online);
      EndSession(ReplacedMessage);
      EmitText(ERedwoodSessionEffectKind::BroadcastAuthFailed, ReplacedMessage);
      return;
    default:
      // No channel exists in the other phases.
      Ignore(Event);
      return;
  }
}

void FRedwoodClientSession::OnLoginReply(const FRedwoodSessionEvent &Event) {
  // A reply of an old join cannot act. While logging out any outcome of the
  // join in flight is taken, because the session it may bring must end.
  if (Phase != ERedwoodSessionPhase::LoggingOut && Event.Seq != JoinSeq) {
    Ignore(Event);
    return;
  }

  switch (Phase) {
    case ERedwoodSessionPhase::LoggingIn: {
      const uint32 Request = PendingLoginRequest;
      PendingLoginRequest = 0;
      if (Event.Outcome == ERedwoodReplyOutcome::Ok) {
        TakeLogin(Event);
        RealmId.Reset();
        Go(ERedwoodSessionPhase::LoggedIn);
        if (bPendingRememberMe) {
          FRedwoodSessionEffect &Save =
            Emit(ERedwoodSessionEffectKind::SaveCredentials);
          Save.Username = PendingLoginUsername;
          Save.Secret = AuthToken;
        } else {
          Emit(ERedwoodSessionEffectKind::ClearCredentials);
        }
        EmitAnswerLogin(Request, ERedwoodLoginAnswer::Success, FString());
        return;
      }
      Go(ERedwoodSessionPhase::Online);
      if (Event.Outcome == ERedwoodReplyOutcome::Lost) {
        EmitAnswerLogin(
          Request,
          ERedwoodLoginAnswer::Error,
          FRedwoodPendingReplies::LostReplyError
        );
        return;
      }
      if (Event.Message == MustVerifyMessage) {
        EmitAnswerLogin(Request, ERedwoodLoginAnswer::MustVerify, FString());
        return;
      }
      Emit(ERedwoodSessionEffectKind::ClearCredentials);
      EmitAnswerLogin(
        Request,
        ERedwoodLoginAnswer::Error,
        Event.Message.IsEmpty() ? JoinRefusalText(Event.Reason) : Event.Message
      );
      return;
    }
    case ERedwoodSessionPhase::Relogging:
      switch (Event.Outcome) {
        case ERedwoodReplyOutcome::Ok: {
          TakeLogin(Event);
          FRedwoodSessionEffect &Save =
            Emit(ERedwoodSessionEffectKind::SaveCredentials);
          Save.Username = PlayerId;
          Save.Secret = AuthToken;
          if (!RealmId.IsEmpty()) {
            Go(ERedwoodSessionPhase::Reentering);
            EmitSendEnterRealm(RealmId);
            return;
          }
          Restore(ERedwoodSessionPhase::LoggedIn);
          return;
        }
        case ERedwoodReplyOutcome::Refused: {
          // The token is dead: the session cannot come back.
          const FString Message = Event.Message.IsEmpty()
            ? JoinRefusalText(Event.Reason)
            : Event.Message;
          Go(ERedwoodSessionPhase::Online);
          EndSession(Message);
          EmitText(ERedwoodSessionEffectKind::BroadcastAuthFailed, Message);
          return;
        }
        case ERedwoodReplyOutcome::Lost:
          // The socket went with the join; the next socket logs in again.
          Go(ERedwoodSessionPhase::ReloginPending);
          return;
      }
      return;
    case ERedwoodSessionPhase::LoggingOut:
      Go(ERedwoodSessionPhase::Online);
      if (Event.Outcome == ERedwoodReplyOutcome::Ok) {
        // The join made a session for a player who left: end it.
        Emit(ERedwoodSessionEffectKind::SendLogoutStep);
        Emit(ERedwoodSessionEffectKind::ForgetSession);
      }
      return;
    default:
      Ignore(Event);
      return;
  }
}

void FRedwoodClientSession::OnEnterRealmReply(
  const FRedwoodSessionEvent &Event
) {
  if (Event.Seq != StepSeq) {
    Ignore(Event);
    return;
  }

  switch (Phase) {
    case ERedwoodSessionPhase::EnteringRealm:
      switch (Event.Outcome) {
        case ERedwoodReplyOutcome::Ok:
          Go(ERedwoodSessionPhase::InRealm);
          Emit(ERedwoodSessionEffectKind::ApplyRegions).Response =
            Event.Response;
          Emit(ERedwoodSessionEffectKind::RealmEntered);
          if (!SelectedCharacterId.IsEmpty()) {
            EmitText(
              ERedwoodSessionEffectKind::SendSelectCharacter,
              SelectedCharacterId
            );
          }
          AnswerPendingEntries(FString());
          return;
        case ERedwoodReplyOutcome::Refused:
          // The realm holds nothing: the session is free to enter another.
          RealmId.Reset();
          Go(ERedwoodSessionPhase::LoggedIn);
          Emit(ERedwoodSessionEffectKind::RealmCleared);
          AnswerPendingEntries(Event.Message);
          return;
        case ERedwoodReplyOutcome::Lost:
          // The socket went with the step. Whether the realm started the
          // session is unknown; the resume's reply names the locked realm.
          Suspend(FString());
          AnswerPendingEntries(FRedwoodPendingReplies::LostReplyError);
          return;
      }
      return;
    case ERedwoodSessionPhase::Reentering:
      switch (Event.Outcome) {
        case ERedwoodReplyOutcome::Ok:
          // The target is the realm now.
          Emit(ERedwoodSessionEffectKind::ApplyRegions).Response =
            Event.Response;
          Emit(ERedwoodSessionEffectKind::RealmEntered);
          // Before the held requests, which can act for the character.
          if (!SelectedCharacterId.IsEmpty()) {
            EmitText(
              ERedwoodSessionEffectKind::SendSelectCharacter,
              SelectedCharacterId
            );
          }
          AnswerPendingEntries(FString());
          Restore(ERedwoodSessionPhase::InRealm);
          return;
        case ERedwoodReplyOutcome::Refused: {
          // The realm refused the session it had: the session cannot come
          // back as it was. The new login is logged out of Bifrost.
          const FString Error = Event.Message;
          Go(ERedwoodSessionPhase::Online);
          Emit(ERedwoodSessionEffectKind::SendLogoutStep);
          EndSession(Error);
          EmitText(ERedwoodSessionEffectKind::BroadcastAuthFailed, Error);
          return;
        }
        case ERedwoodReplyOutcome::Lost:
          // The socket went with the step; the next socket resumes the
          // session, and Bifrost names the locked realm.
          Go(ERedwoodSessionPhase::Suspended);
          AnswerPendingEntries(FRedwoodPendingReplies::LostReplyError);
          return;
      }
      return;
    default:
      Ignore(Event);
      return;
  }
}

void FRedwoodClientSession::OnGraceExpired() {
  if (!IsComingBack()) {
    Ignore(FRedwoodSessionEvent::GraceExpired());
    return;
  }
  // The held requests fail now, and later ones fail at once (Gate) until
  // the session is back.
  bGraceExpired = true;
  Emit(ERedwoodSessionEffectKind::ExpireHeld);
}

void FRedwoodClientSession::OnInitialize() {
  // A new connection. Whatever session or join there was goes with the old
  // one, with no word to the game: the game asked.
  if (
    HasPlayerSession() || Phase == ERedwoodSessionPhase::LoggingIn ||
    Phase == ERedwoodSessionPhase::LoggingOut
  ) {
    EndSession(EntryNotReadyError);
  }
  if (PendingLoginRequest != 0) {
    EmitAnswerLogin(
      PendingLoginRequest, ERedwoodLoginAnswer::Error, NotConnectedToDirector
    );
    PendingLoginRequest = 0;
  }
  Go(ERedwoodSessionPhase::Connecting);
}

void FRedwoodClientSession::OnLogin(const FRedwoodSessionEvent &Event) {
  switch (Phase) {
    case ERedwoodSessionPhase::Connecting:
    case ERedwoodSessionPhase::ConnectFailed:
    case ERedwoodSessionPhase::Offline:
    case ERedwoodSessionPhase::OfflineQuiet:
      EmitAnswerLogin(
        Event.RequestId, ERedwoodLoginAnswer::Error, NotConnectedToDirector
      );
      return;
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
      // A new login ends the session that was coming back, and there is no
      // socket to send the new one on.
      Go(ERedwoodSessionPhase::Offline);
      EndSession(EntryNotReadyError);
      EmitAnswerLogin(
        Event.RequestId, ERedwoodLoginAnswer::Error, NotConnectedToDirector
      );
      return;
    case ERedwoodSessionPhase::Online:
      break;
    case ERedwoodSessionPhase::LoggingIn: {
      // The connection replaces the pending join; its caller hears it here.
      const uint32 Replaced = PendingLoginRequest;
      PendingLoginRequest = 0;
      EndSession(EntryNotReadyError);
      EmitAnswerLogin(Replaced, ERedwoodLoginAnswer::Error, NewerLoginError);
      break;
    }
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Relogging:
    case ERedwoodSessionPhase::LoggingOut:
      EndSession(EntryNotReadyError);
      break;
    case ERedwoodSessionPhase::InRealm:
      Emit(ERedwoodSessionEffectKind::PayOwedLeave);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      break;
    case ERedwoodSessionPhase::LoggedIn:
    case ERedwoodSessionPhase::EnteringRealm:
    case ERedwoodSessionPhase::Reentering:
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      break;
    default:
      break;
  }

  // A new session starts clean, whatever the last one was in.
  Go(ERedwoodSessionPhase::LoggingIn);
  PendingLoginRequest = Event.RequestId;
  PendingLoginUsername = Event.Username;
  bPendingRememberMe = Event.bRememberMe;
  EmitSendLogin(Event.Username, Event.Secret, Event.Provider);
}

void FRedwoodClientSession::OnLogout() {
  switch (Phase) {
    case ERedwoodSessionPhase::LoggingIn: {
      // The join in flight may still make a session; its reply ends it.
      const uint32 Request = PendingLoginRequest;
      PendingLoginRequest = 0;
      Go(ERedwoodSessionPhase::LoggingOut);
      EndSession(LoggedOutError);
      EmitAnswerLogin(Request, ERedwoodLoginAnswer::Error, LoggedOutError);
      return;
    }
    case ERedwoodSessionPhase::LoggedIn:
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      return;
    case ERedwoodSessionPhase::EnteringRealm:
      Go(ERedwoodSessionPhase::Online);
      AnswerPendingEntries(LoggedOutError);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      return;
    case ERedwoodSessionPhase::InRealm:
      // The owed leave first: only a realm that knows the player runs it.
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::PayOwedLeave);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      return;
    case ERedwoodSessionPhase::Suspended:
    case ERedwoodSessionPhase::ReloginPending:
      // Nothing to send on a socket that is down; the session Bifrost keeps
      // ends after its grace, and no resume brings it back.
      Go(ERedwoodSessionPhase::Offline);
      EndSession(LoggedOutError);
      return;
    case ERedwoodSessionPhase::Resuming:
    case ERedwoodSessionPhase::Relogging:
      // The join's reply ends the session it brings.
      Go(ERedwoodSessionPhase::LoggingOut);
      EndSession(LoggedOutError);
      return;
    case ERedwoodSessionPhase::Reentering:
      // The entry step is in flight; the logout queues behind it, and the
      // entry's reply is of an old step by then.
      Go(ERedwoodSessionPhase::Online);
      Emit(ERedwoodSessionEffectKind::SendLogoutStep);
      EndSession(LoggedOutError);
      return;
    default:
      // No session: Connecting, ConnectFailed, Online, Offline,
      // OfflineQuiet, LoggingOut.
      Ignore(FRedwoodSessionEvent::Logout());
      return;
  }
}

void FRedwoodClientSession::OnEnterRealm(const FRedwoodSessionEvent &Event) {
  switch (Phase) {
    case ERedwoodSessionPhase::LoggedIn:
      RealmId = Event.RealmId;
      Go(ERedwoodSessionPhase::EnteringRealm);
      Emit(ERedwoodSessionEffectKind::RealmCleared);
      PendingEntryRequests.Add(Event.RequestId);
      EmitSendEnterRealm(Event.RealmId);
      return;
    case ERedwoodSessionPhase::InRealm:
      // The session is in this realm already (the game asks again after an
      // entry whose answer was lost with the socket, and the resume entered
      // it). A second start at the realm would replace the session with
      // itself, so the answer is given here. Another realm needs a new
      // login: to leave a realm is an end that must reach it.
      EmitText(
        ERedwoodSessionEffectKind::AnswerRealmEntry,
        Event.RealmId == RealmId ? FString() : FString(OtherRealmError)
      )
        .RequestId = Event.RequestId;
      return;
    case ERedwoodSessionPhase::EnteringRealm:
    case ERedwoodSessionPhase::Reentering:
      if (Event.RealmId == RealmId) {
        // The same entry is in flight; this caller hears its answer.
        PendingEntryRequests.Add(Event.RequestId);
        return;
      }
      EmitText(
        ERedwoodSessionEffectKind::AnswerRealmEntry, EntryInProgressError
      )
        .RequestId = Event.RequestId;
      return;
    default:
      EmitText(ERedwoodSessionEffectKind::AnswerRealmEntry, EntryNotReadyError)
        .RequestId = Event.RequestId;
      return;
  }
}

void FRedwoodClientSession::OnSelectCharacter(
  const FRedwoodSessionEvent &Event
) {
  SelectedCharacterId = Event.CharacterId;
  // In any other phase the selection goes out when the session arrives in
  // its realm (OnResumeAccepted, OnEnterRealmReply).
  if (Phase == ERedwoodSessionPhase::InRealm) {
    EmitText(
      ERedwoodSessionEffectKind::SendSelectCharacter, SelectedCharacterId
    );
  }
}

// ── Building blocks ─────────────────────────────────────────────────────

void FRedwoodClientSession::Go(ERedwoodSessionPhase To) {
  Phase = To;
}

FRedwoodSessionEffect &FRedwoodClientSession::Emit(
  ERedwoodSessionEffectKind Kind
) {
  return Out.Emplace_GetRef(Kind);
}

FRedwoodSessionEffect &FRedwoodClientSession::EmitText(
  ERedwoodSessionEffectKind Kind, const FString &Text
) {
  FRedwoodSessionEffect &Effect = Emit(Kind);
  Effect.Text = Text;
  return Effect;
}

void FRedwoodClientSession::EmitSendLogin(
  const FString &Username, const FString &Secret, const FString &Provider
) {
  FRedwoodSessionEffect &Effect = Emit(ERedwoodSessionEffectKind::SendLogin);
  Effect.Seq = ++JoinSeq;
  Effect.Username = Username;
  Effect.Secret = Secret;
  Effect.Provider = Provider;
}

void FRedwoodClientSession::EmitSendEnterRealm(const FString &InRealmId) {
  FRedwoodSessionEffect &Effect =
    Emit(ERedwoodSessionEffectKind::SendEnterRealm);
  Effect.Seq = ++StepSeq;
  Effect.Text = InRealmId;
}

void FRedwoodClientSession::EmitAnswerLogin(
  uint32 RequestId, ERedwoodLoginAnswer Answer, const FString &Message
) {
  if (RequestId == 0) {
    return;
  }
  FRedwoodSessionEffect &Effect = Emit(ERedwoodSessionEffectKind::AnswerLogin);
  Effect.RequestId = RequestId;
  Effect.LoginAnswer = Answer;
  Effect.Text = Message;
}

void FRedwoodClientSession::AnswerPendingEntries(const FString &Error) {
  for (const uint32 RequestId : PendingEntryRequests) {
    EmitText(ERedwoodSessionEffectKind::AnswerRealmEntry, Error).RequestId =
      RequestId;
  }
  PendingEntryRequests.Reset();
}

void FRedwoodClientSession::Suspend(const FString &RealmToRestore) {
  RealmId = RealmToRestore;
  bGraceExpired = false;
  Go(ERedwoodSessionPhase::Suspended);
  // The grace counts from the drop, like the game's own disconnect grace.
  Emit(ERedwoodSessionEffectKind::StartGrace);
  // Last among the non-game effects, so a request that a failure handler
  // sends again is held for the reconnect.
  Emit(ERedwoodSessionEffectKind::FailPendingReplies);
}

void FRedwoodClientSession::Restore(ERedwoodSessionPhase To) {
  bGraceExpired = false;
  Go(To);
  Emit(ERedwoodSessionEffectKind::ReleaseHeld);
  Emit(ERedwoodSessionEffectKind::StopGrace);
  Emit(ERedwoodSessionEffectKind::BroadcastReestablished);
}

void FRedwoodClientSession::EndSession(const FString &EntryError) {
  AnswerPendingEntries(EntryError);
  Emit(ERedwoodSessionEffectKind::FailPendingReplies);
  Emit(ERedwoodSessionEffectKind::ExpireHeld);
  Emit(ERedwoodSessionEffectKind::StopGrace);
  Emit(ERedwoodSessionEffectKind::ForgetSession);
  Emit(ERedwoodSessionEffectKind::SessionCleared);
  Emit(ERedwoodSessionEffectKind::RealmCleared);
  Emit(ERedwoodSessionEffectKind::ClearCredentials);
  PlayerId.Reset();
  AuthToken.Reset();
  Nickname.Reset();
  RealmId.Reset();
  SelectedCharacterId.Reset();
  PendingLoginUsername.Reset();
  bPendingRememberMe = false;
  bGraceExpired = false;
}

void FRedwoodClientSession::TakeLogin(const FRedwoodSessionEvent &Event) {
  PlayerId = Event.PlayerId;
  AuthToken = Event.Token;
  Nickname = Event.Nickname;
}

void FRedwoodClientSession::Ignore(const FRedwoodSessionEvent &Event) {
  UE_LOG(
    LogRedwood,
    Verbose,
    TEXT("Session event %s changes nothing in phase %s."),
    EventName(Event.Kind),
    PhaseName(Phase)
  );
}

// The anonymous namespace of this file would collide with the client
// interface's copy of this text in a unity build, so it lives on the class.
FString FRedwoodClientSession::JoinRefusalText(const FString &Reason) {
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

// ── Names ───────────────────────────────────────────────────────────────

const TCHAR *FRedwoodClientSession::PhaseName(ERedwoodSessionPhase InPhase) {
  switch (InPhase) {
    case ERedwoodSessionPhase::Connecting:
      return TEXT("Connecting");
    case ERedwoodSessionPhase::ConnectFailed:
      return TEXT("ConnectFailed");
    case ERedwoodSessionPhase::Online:
      return TEXT("Online");
    case ERedwoodSessionPhase::Offline:
      return TEXT("Offline");
    case ERedwoodSessionPhase::OfflineQuiet:
      return TEXT("OfflineQuiet");
    case ERedwoodSessionPhase::LoggingIn:
      return TEXT("LoggingIn");
    case ERedwoodSessionPhase::LoggedIn:
      return TEXT("LoggedIn");
    case ERedwoodSessionPhase::EnteringRealm:
      return TEXT("EnteringRealm");
    case ERedwoodSessionPhase::InRealm:
      return TEXT("InRealm");
    case ERedwoodSessionPhase::Suspended:
      return TEXT("Suspended");
    case ERedwoodSessionPhase::ReloginPending:
      return TEXT("ReloginPending");
    case ERedwoodSessionPhase::Resuming:
      return TEXT("Resuming");
    case ERedwoodSessionPhase::Relogging:
      return TEXT("Relogging");
    case ERedwoodSessionPhase::Reentering:
      return TEXT("Reentering");
    case ERedwoodSessionPhase::LoggingOut:
      return TEXT("LoggingOut");
    default:
      return TEXT("?");
  }
}

const TCHAR *FRedwoodClientSession::EventName(ERedwoodSessionEventKind Kind) {
  switch (Kind) {
    case ERedwoodSessionEventKind::SocketUp:
      return TEXT("SocketUp");
    case ERedwoodSessionEventKind::SocketDown:
      return TEXT("SocketDown");
    case ERedwoodSessionEventKind::ResumeAccepted:
      return TEXT("ResumeAccepted");
    case ERedwoodSessionEventKind::ResumeRefused:
      return TEXT("ResumeRefused");
    case ERedwoodSessionEventKind::ClosedByServer:
      return TEXT("ClosedByServer");
    case ERedwoodSessionEventKind::LoginReply:
      return TEXT("LoginReply");
    case ERedwoodSessionEventKind::EnterRealmReply:
      return TEXT("EnterRealmReply");
    case ERedwoodSessionEventKind::GraceExpired:
      return TEXT("GraceExpired");
    case ERedwoodSessionEventKind::Initialize:
      return TEXT("Initialize");
    case ERedwoodSessionEventKind::Login:
      return TEXT("Login");
    case ERedwoodSessionEventKind::Logout:
      return TEXT("Logout");
    case ERedwoodSessionEventKind::EnterRealm:
      return TEXT("EnterRealm");
    case ERedwoodSessionEventKind::SelectCharacter:
      return TEXT("SelectCharacter");
    default:
      return TEXT("?");
  }
}

const TCHAR *FRedwoodClientSession::EffectName(ERedwoodSessionEffectKind Kind) {
  switch (Kind) {
    case ERedwoodSessionEffectKind::SendLogin:
      return TEXT("SendLogin");
    case ERedwoodSessionEffectKind::SendEnterRealm:
      return TEXT("SendEnterRealm");
    case ERedwoodSessionEffectKind::SendSelectCharacter:
      return TEXT("SendSelectCharacter");
    case ERedwoodSessionEffectKind::SendLogoutStep:
      return TEXT("SendLogoutStep");
    case ERedwoodSessionEffectKind::ForgetSession:
      return TEXT("ForgetSession");
    case ERedwoodSessionEffectKind::StartGrace:
      return TEXT("StartGrace");
    case ERedwoodSessionEffectKind::StopGrace:
      return TEXT("StopGrace");
    case ERedwoodSessionEffectKind::ReleaseHeld:
      return TEXT("ReleaseHeld");
    case ERedwoodSessionEffectKind::ExpireHeld:
      return TEXT("ExpireHeld");
    case ERedwoodSessionEffectKind::FailPendingReplies:
      return TEXT("FailPendingReplies");
    case ERedwoodSessionEffectKind::BroadcastLost:
      return TEXT("BroadcastLost");
    case ERedwoodSessionEffectKind::BroadcastReestablished:
      return TEXT("BroadcastReestablished");
    case ERedwoodSessionEffectKind::BroadcastAuthFailed:
      return TEXT("BroadcastAuthFailed");
    case ERedwoodSessionEffectKind::ReportFirstConnect:
      return TEXT("ReportFirstConnect");
    case ERedwoodSessionEffectKind::LogFirstConnectFailure:
      return TEXT("LogFirstConnectFailure");
    case ERedwoodSessionEffectKind::AnswerLogin:
      return TEXT("AnswerLogin");
    case ERedwoodSessionEffectKind::AnswerRealmEntry:
      return TEXT("AnswerRealmEntry");
    case ERedwoodSessionEffectKind::ApplyRegions:
      return TEXT("ApplyRegions");
    case ERedwoodSessionEffectKind::RealmEntered:
      return TEXT("RealmEntered");
    case ERedwoodSessionEffectKind::PayOwedLeave:
      return TEXT("PayOwedLeave");
    case ERedwoodSessionEffectKind::RealmCleared:
      return TEXT("RealmCleared");
    case ERedwoodSessionEffectKind::SessionCleared:
      return TEXT("SessionCleared");
    case ERedwoodSessionEffectKind::SaveCredentials:
      return TEXT("SaveCredentials");
    case ERedwoodSessionEffectKind::ClearCredentials:
      return TEXT("ClearCredentials");
    default:
      return TEXT("?");
  }
}
