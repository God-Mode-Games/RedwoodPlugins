// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// The life of the player's session on the one connection to Bifrost, as one
// state machine. URedwoodClientInterface feeds it events (what the socket,
// Bifrost and the game did) and runs the effects it returns (what to send,
// what to hold or fail, what to tell the game). The machine itself has no
// socket, no timer and no UObject, so a table test can walk every phase and
// every event.
//
// Why a machine: the same facts used to live in nine booleans written by
// eight callbacks, and every review found a wrong combination. Here each
// reachable combination of socket, session and realm is one phase, and the
// three game events are structural: a lost connection is broadcast when the
// phase enters an outage, a reestablished one when it leaves an outage with
// the session back, an authentication failure when a session ends by refusal
// or replacement. Nothing else can broadcast them.
//
// Phases, with what each replaces of the old flags:
//   Connecting      first socket not up yet, nothing reported
//   ConnectFailed   first socket not up yet; a loss was broadcast
//   Online          socket up, no session (the title screen)
//   Offline         socket down, no session; a loss was broadcast
//   OfflineQuiet    socket closed by Bifrost after `closed`; no loss, the
//                   socket comes back for the next login
//   LoggingIn       the game's login join is in flight
//   LoggedIn        session live, in no realm
//   EnteringRealm   `session.enter-realm` in flight
//   InRealm         realm entered
//   Suspended       socket down, Bifrost keeps the session; requests held
//   ReloginPending  socket down, no Bifrost session; a token to log in with
//   Resuming        resume join in flight
//   Relogging       token login in flight (the resume was refused or lost)
//   Reentering      re-login accepted; enter-realm for the old realm in flight
//   LoggingOut      the player logged out while a join was in flight; that
//                   join's reply ends the session it brings
enum class ERedwoodSessionPhase : uint8 {
  Connecting,
  ConnectFailed,
  Online,
  Offline,
  OfflineQuiet,
  LoggingIn,
  LoggedIn,
  EnteringRealm,
  InRealm,
  Suspended,
  ReloginPending,
  Resuming,
  Relogging,
  Reentering,
  LoggingOut,
  Count
};

enum class ERedwoodSessionEventKind : uint8 {
  // From the connection.
  SocketUp,
  SocketDown,
  ResumeAccepted,
  ResumeRefused,
  ClosedByServer,
  LoginReply,
  EnterRealmReply,
  // From the adapter's grace timer.
  GraceExpired,
  // From the game.
  Initialize,
  Login,
  Logout,
  EnterRealm,
  SelectCharacter,
  Count
};

// How a join or a step was answered.
enum class ERedwoodReplyOutcome : uint8 { Ok, Refused, Lost };

struct REDWOOD_API FRedwoodSessionEvent {
  ERedwoodSessionEventKind Kind = ERedwoodSessionEventKind::SocketUp;

  // LoginReply, EnterRealmReply: the sequence the adapter tagged the message
  // with (from the SendLogin / SendEnterRealm effect), and how it ended.
  uint32 Seq = 0;
  ERedwoodReplyOutcome Outcome = ERedwoodReplyOutcome::Ok;

  // Login, EnterRealm: the adapter's id of the game's call, echoed in the
  // AnswerLogin / AnswerRealmEntry effect so the adapter finds the caller.
  uint32 RequestId = 0;

  // SocketDown: false when Bifrost closed the socket after `closed`.
  bool bAnnounced = true;

  // LoginReply Ok: PlayerId, Token, Nickname. LoginReply Refused: Reason,
  // Message. ResumeAccepted: RealmId, LockedRealmId, CharacterId.
  // ClosedByServer: Reason. EnterRealmReply Refused: Message (the realm's
  // error). EnterRealmReply Ok: Response (the realm's answer, for the
  // regions). Login: Username, Secret, Provider, bRememberMe. EnterRealm:
  // RealmId. SelectCharacter: CharacterId.
  FString PlayerId;
  FString Token;
  FString Nickname;
  FString Reason;
  FString Message;
  FString RealmId;
  FString LockedRealmId;
  FString CharacterId;
  FString Username;
  FString Secret;
  FString Provider;
  bool bRememberMe = false;
  TSharedPtr<FJsonObject> Response;

  static FRedwoodSessionEvent SocketUp();
  static FRedwoodSessionEvent SocketDown(bool bAnnounced = true);
  static FRedwoodSessionEvent ResumeAccepted(
    const FString &RealmId,
    const FString &LockedRealmId,
    const FString &CharacterId
  );
  static FRedwoodSessionEvent ResumeRefused();
  static FRedwoodSessionEvent ClosedByServer(const FString &Reason);
  static FRedwoodSessionEvent LoginOk(
    uint32 Seq,
    const FString &PlayerId,
    const FString &Token,
    const FString &Nickname
  );
  // PlayerId is present when the account must verify its email: the push
  // that confirms it names the player.
  static FRedwoodSessionEvent LoginRefused(
    uint32 Seq,
    const FString &Reason,
    const FString &Message,
    const FString &PlayerId = FString()
  );
  static FRedwoodSessionEvent LoginLost(uint32 Seq);
  static FRedwoodSessionEvent EnterRealmOk(
    uint32 Seq, const TSharedPtr<FJsonObject> &Response
  );
  static FRedwoodSessionEvent EnterRealmRefused(
    uint32 Seq, const FString &Error
  );
  static FRedwoodSessionEvent EnterRealmLost(uint32 Seq);
  static FRedwoodSessionEvent GraceExpired();
  static FRedwoodSessionEvent Initialize();
  static FRedwoodSessionEvent Login(
    uint32 RequestId,
    const FString &Username,
    const FString &Secret,
    const FString &Provider,
    bool bRememberMe
  );
  static FRedwoodSessionEvent Logout();
  static FRedwoodSessionEvent EnterRealm(
    uint32 RequestId, const FString &RealmId
  );
  static FRedwoodSessionEvent SelectCharacter(const FString &CharacterId);
};

enum class ERedwoodSessionEffectKind : uint8 {
  // To Bifrost, through the connection.
  SendLogin, // Seq, Username, Secret, Provider
  SendEnterRealm, // Seq, Text = realm id
  SendSelectCharacter, // Text = character id
  SendLogoutStep,
  ForgetSession,
  // The session came back: `chat.resume` with the cursors, so the lines
  // missed during the gap are replayed. Before ReleaseHeld, so a held chat
  // send goes out behind the repair, and the player's own line lands after
  // the lines they missed.
  ResumeChat,
  // The held requests and the pending replies.
  StartGrace,
  StopGrace,
  ReleaseHeld,
  ExpireHeld,
  FailPendingReplies,
  // To the game.
  BroadcastLost,
  BroadcastReestablished,
  BroadcastAuthFailed, // Text = message
  ReportFirstConnect,
  LogFirstConnectFailure,
  AnswerLogin, // RequestId, LoginAnswer, Text = message, or the player id
  // of an account that must verify its email
  AnswerRealmEntry, // RequestId, Text = error ("" is success)
  // To the adapter's own state.
  ApplyRegions, // Response
  RealmEntered, // the ticketing cluster sends its owed leave
  PayOwedLeave, // Text = player id; before a logout step that leaves a realm.
  // The ids are cleared before the effects run, so the leave
  // carries the player it is for.
  RealmCleared, // the ticketing cluster forgets the realm
  SessionCleared, // the adapter forgets what is not the machine's
  SaveCredentials, // Username, Secret = token
  ClearCredentials
};

enum class ERedwoodLoginAnswer : uint8 { Success, Error, MustVerify };

struct REDWOOD_API FRedwoodSessionEffect {
  ERedwoodSessionEffectKind Kind = ERedwoodSessionEffectKind::ForgetSession;
  uint32 Seq = 0;
  uint32 RequestId = 0;
  ERedwoodLoginAnswer LoginAnswer = ERedwoodLoginAnswer::Success;
  FString Text;
  FString Username;
  FString Secret;
  FString Provider;
  TSharedPtr<FJsonObject> Response;

  explicit FRedwoodSessionEffect(ERedwoodSessionEffectKind InKind) :
    Kind(InKind) {}

  // Tests compare effects by their kind and their text fields.
  bool operator==(const FRedwoodSessionEffect &Other) const;
  FString ToString() const;
};

enum class ERedwoodRequestKind : uint8 { Director, Realm };
enum class ERedwoodGateDecision : uint8 { Send, Hold, Fail };

class REDWOOD_API FRedwoodClientSession {
public:
  // The message a request gets when the connection is not there to carry
  // it, as the socket-library version of the client gave it.
  static const TCHAR *const NotConnectedToDirector;
  static const TCHAR *const NotConnectedToRealm;
  static const TCHAR *const LoggedOutError;
  static const TCHAR *const ReplacedMessage;
  static const TCHAR *const NewerLoginError;
  static const TCHAR *const OtherRealmError;
  static const TCHAR *const EntryInProgressError;
  static const TCHAR *const EntryNotReadyError;

  // Applies the event: the phase is committed before any effect runs, and
  // the effects come back in the order to run them. A caller that re-enters
  // from a game-facing effect (Answer*, Broadcast*) sees the new phase, and
  // the adapter queues the event it raises until these effects are done.
  TArray<FRedwoodSessionEffect> Handle(const FRedwoodSessionEvent &Event);

  ERedwoodSessionPhase GetPhase() const {
    return Phase;
  }

  // ── Queries: pure functions of the phase ────────────────────────────

  // The socket is up (the game's "director connected").
  bool IsSocketUp() const;
  // The session is live on the channel.
  bool IsLoggedIn() const;
  // A session exists: live, or coming back after a drop.
  bool HasPlayerSession() const;
  bool IsRealmEntered() const;
  // A loss was broadcast and nothing good since.
  bool IsOutage() const;
  // The socket dropped and the session is not back yet; requests are held.
  bool IsComingBack() const;
  bool IsGraceExpired() const {
    return bGraceExpired;
  }
  ERedwoodGateDecision Gate(ERedwoodRequestKind Kind) const;

  const FString &GetPlayerId() const {
    return PlayerId;
  }
  const FString &GetAuthToken() const {
    return AuthToken;
  }
  const FString &GetNickname() const {
    return Nickname;
  }
  // The realm the session is in. Empty while an entry is in flight; the
  // realm to restore while the session is coming back.
  FString GetRealmId() const;
  const FString &GetSelectedCharacterId() const {
    return SelectedCharacterId;
  }
  uint32 GetJoinSeq() const {
    return JoinSeq;
  }
  uint32 GetStepSeq() const {
    return StepSeq;
  }

  static bool IsOutagePhase(ERedwoodSessionPhase InPhase);
  static bool IsComingBackPhase(ERedwoodSessionPhase InPhase);
  static bool IsLoggedInPhase(ERedwoodSessionPhase InPhase);
  static bool IsSocketUpPhase(ERedwoodSessionPhase InPhase);
  // What the player reads for a join that Bifrost refused with a reason and
  // no message of its own (protocol.md, "Join").
  static FString JoinRefusalText(const FString &Reason);
  static const TCHAR *PhaseName(ERedwoodSessionPhase InPhase);
  static const TCHAR *EventName(ERedwoodSessionEventKind Kind);
  static const TCHAR *EffectName(ERedwoodSessionEffectKind Kind);

private:
  void OnSocketUp();
  void OnSocketDown(const FRedwoodSessionEvent &Event);
  void OnResumeAccepted(const FRedwoodSessionEvent &Event);
  void OnResumeRefused();
  void OnClosedByServer(const FRedwoodSessionEvent &Event);
  void OnLoginReply(const FRedwoodSessionEvent &Event);
  void OnEnterRealmReply(const FRedwoodSessionEvent &Event);
  void OnGraceExpired();
  void OnInitialize();
  void OnLogin(const FRedwoodSessionEvent &Event);
  void OnLogout();
  void OnEnterRealm(const FRedwoodSessionEvent &Event);
  void OnSelectCharacter(const FRedwoodSessionEvent &Event);

  // Building blocks of the rows.
  void Go(ERedwoodSessionPhase To);
  FRedwoodSessionEffect &Emit(ERedwoodSessionEffectKind Kind);
  FRedwoodSessionEffect &EmitText(
    ERedwoodSessionEffectKind Kind, const FString &Text
  );
  void EmitSendLogin(
    const FString &Username, const FString &Secret, const FString &Provider
  );
  void EmitSendEnterRealm(const FString &InRealmId);
  void EmitAnswerLogin(
    uint32 RequestId, ERedwoodLoginAnswer Answer, const FString &Message
  );
  void AnswerPendingEntries(const FString &Error);
  // The socket is gone with a session to bring back.
  void Suspend(const FString &RealmToRestore);
  // The session is back on the channel.
  void Restore(ERedwoodSessionPhase To);
  // Everything of the session goes; the realm with it. The game's realm
  // entries still waiting hear EntryError.
  void EndSession(const FString &EntryError);
  void TakeLogin(const FRedwoodSessionEvent &Event);
  void Ignore(const FRedwoodSessionEvent &Event);

  ERedwoodSessionPhase Phase = ERedwoodSessionPhase::Connecting;
  FString PlayerId;
  FString AuthToken;
  FString Nickname;
  // InRealm: entered. EnteringRealm, Reentering: the target. Suspended,
  // ReloginPending, Resuming, Relogging: to restore, or empty.
  FString RealmId;
  FString SelectedCharacterId;
  // The current join and the current enter-realm step; a reply with another
  // sequence is of an old one and cannot act.
  uint32 JoinSeq = 0;
  uint32 StepSeq = 0;
  // The game's login waiting for its answer, and its remember-me wish.
  uint32 PendingLoginRequest = 0;
  FString PendingLoginUsername;
  bool bPendingRememberMe = false;
  // The game's realm entries waiting on the step in flight.
  TArray<uint32> PendingEntryRequests;
  // Only in the coming-back phases: the held requests' grace ran out, so
  // new requests fail instead of waiting.
  bool bGraceExpired = false;

  TArray<FRedwoodSessionEffect> Out;
};
