// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins FRedwoodClientSession, the one state machine of the player's session
// on the connection to Bifrost. The machine has no socket and no timer, so
// these tests walk it directly:
//   1. Table: every phase against every event variant. A cell with a row
//      must give that row's phase and effects, in order; a cell without one
//      must change nothing and emit nothing.
//   2. Invariants over every row: a lost connection is broadcast exactly
//      when the phase enters an outage; a phase that leaves an outage does so
//      with exactly one of a reestablished connection, an authentication
//      failure, or the player's own login, logout or initialize; an
//      authentication failure only ends a session; a session that continues
//      keeps its ids; a session that ends is forgotten on the connection.
//   3. Gate and the queries, as functions of the phase.
//   4. Scripts, one per hole the reviews of the flag version found.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodClientSession.h"
#include "RedwoodPendingReplies.h"

namespace RedwoodSessionMachineTest {
  using EPhase = ERedwoodSessionPhase;
  using EKind = ERedwoodSessionEffectKind;
  using EAnswer = ERedwoodLoginAnswer;

  const FString RealmA = TEXT("realm-1");
  const FString RealmB = TEXT("realm-2");
  const FString CharA = TEXT("char-1");
  const FString CharB = TEXT("char-2");
  const FString CharOther = TEXT("char-0");
  const FString PlayerA = TEXT("p1");
  const FString TokenA = TEXT("tok-1");
  const FString TokenB = TEXT("tok-2");
  const FString Refusal = TEXT("Wrong password");
  const FString RealmRefusal = TEXT("Realm is full");
  constexpr uint32 LoginRequest = 7;
  constexpr uint32 EntryRequest = 9;
  // A sequence no join or step ever had.
  constexpr uint32 OldSeq = 999;

  // The event variants the table is walked with. Each builds its event from
  // the machine, because a reply names the current sequence.
  enum class EVariant : uint8 {
    SocketUp,
    SocketDownAnnounced,
    SocketDownQuiet,
    ResumeAcceptedRealm,
    ResumeAcceptedOtherCharacter,
    ResumeAcceptedLocked,
    ResumeAcceptedNone,
    ResumeRefused,
    ClosedReplaced,
    ClosedReconnect,
    LoginOk,
    LoginRefused,
    LoginMustVerify,
    LoginLost,
    LoginOldSeq,
    EnterOk,
    EnterRefused,
    EnterLost,
    EnterOldSeq,
    GraceExpired,
    Initialize,
    Login,
    Logout,
    EnterRealmSame,
    EnterRealmOther,
    SelectCharacter,
    Count
  };

  const TCHAR *VariantName(EVariant Variant) {
    static const TCHAR *const Names[] = {
      TEXT("SocketUp"),
      TEXT("SocketDownAnnounced"),
      TEXT("SocketDownQuiet"),
      TEXT("ResumeAcceptedRealm"),
      TEXT("ResumeAcceptedOtherCharacter"),
      TEXT("ResumeAcceptedLocked"),
      TEXT("ResumeAcceptedNone"),
      TEXT("ResumeRefused"),
      TEXT("ClosedReplaced"),
      TEXT("ClosedReconnect"),
      TEXT("LoginOk"),
      TEXT("LoginRefused"),
      TEXT("LoginMustVerify"),
      TEXT("LoginLost"),
      TEXT("LoginOldSeq"),
      TEXT("EnterOk"),
      TEXT("EnterRefused"),
      TEXT("EnterLost"),
      TEXT("EnterOldSeq"),
      TEXT("GraceExpired"),
      TEXT("Initialize"),
      TEXT("Login"),
      TEXT("Logout"),
      TEXT("EnterRealmSame"),
      TEXT("EnterRealmOther"),
      TEXT("SelectCharacter"),
    };
    return Names[static_cast<int32>(Variant)];
  }

  FRedwoodSessionEvent MakeEvent(
    EVariant Variant, const FRedwoodClientSession &Session
  ) {
    const uint32 Join = Session.GetJoinSeq();
    const uint32 Step = Session.GetStepSeq();
    switch (Variant) {
      case EVariant::SocketUp:
        return FRedwoodSessionEvent::SocketUp();
      case EVariant::SocketDownAnnounced:
        return FRedwoodSessionEvent::SocketDown(true);
      case EVariant::SocketDownQuiet:
        return FRedwoodSessionEvent::SocketDown(false);
      case EVariant::ResumeAcceptedRealm:
        return FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA);
      case EVariant::ResumeAcceptedOtherCharacter:
        return FRedwoodSessionEvent::ResumeAccepted(
          RealmA, FString(), CharOther
        );
      case EVariant::ResumeAcceptedLocked:
        return FRedwoodSessionEvent::ResumeAccepted(
          FString(), RealmA, FString()
        );
      case EVariant::ResumeAcceptedNone:
        return FRedwoodSessionEvent::ResumeAccepted(
          FString(), FString(), FString()
        );
      case EVariant::ResumeRefused:
        return FRedwoodSessionEvent::ResumeRefused();
      case EVariant::ClosedReplaced:
        return FRedwoodSessionEvent::ClosedByServer(TEXT("replaced"));
      case EVariant::ClosedReconnect:
        return FRedwoodSessionEvent::ClosedByServer(TEXT("reconnect"));
      case EVariant::LoginOk:
        return FRedwoodSessionEvent::LoginOk(
          Join, PlayerA, TokenB, TEXT("Nick")
        );
      case EVariant::LoginRefused:
        return FRedwoodSessionEvent::LoginRefused(
          Join, TEXT("login_failed"), Refusal
        );
      case EVariant::LoginMustVerify:
        return FRedwoodSessionEvent::LoginRefused(
          Join, TEXT("login_failed"), TEXT("Must verify account")
        );
      case EVariant::LoginLost:
        return FRedwoodSessionEvent::LoginLost(Join);
      case EVariant::LoginOldSeq:
        return FRedwoodSessionEvent::LoginOk(
          OldSeq, PlayerA, TokenB, TEXT("Nick")
        );
      case EVariant::EnterOk:
        return FRedwoodSessionEvent::EnterRealmOk(
          Step, MakeShared<FJsonObject>()
        );
      case EVariant::EnterRefused:
        return FRedwoodSessionEvent::EnterRealmRefused(Step, RealmRefusal);
      case EVariant::EnterLost:
        return FRedwoodSessionEvent::EnterRealmLost(Step);
      case EVariant::EnterOldSeq:
        return FRedwoodSessionEvent::EnterRealmOk(
          OldSeq, MakeShared<FJsonObject>()
        );
      case EVariant::GraceExpired:
        return FRedwoodSessionEvent::GraceExpired();
      case EVariant::Initialize:
        return FRedwoodSessionEvent::Initialize();
      case EVariant::Login:
        return FRedwoodSessionEvent::Login(
          LoginRequest, PlayerA, TEXT("secret"), TEXT("local"), false
        );
      case EVariant::Logout:
        return FRedwoodSessionEvent::Logout();
      case EVariant::EnterRealmSame:
        return FRedwoodSessionEvent::EnterRealm(EntryRequest, RealmA);
      case EVariant::EnterRealmOther:
        return FRedwoodSessionEvent::EnterRealm(EntryRequest, RealmB);
      case EVariant::SelectCharacter:
      default:
        return FRedwoodSessionEvent::SelectCharacter(CharB);
    }
  }

  // Reaches each phase through one canonical path from a fresh machine, so
  // every phase is proven reachable and every row is tested on the same
  // data: player p1, realm realm-1, character char-1 once in the realm.
  FRedwoodClientSession MakeIn(EPhase Phase) {
    FRedwoodClientSession S;
    auto Feed = [&S](const FRedwoodSessionEvent &Event) { S.Handle(Event); };
    auto Up = [&]() { Feed(FRedwoodSessionEvent::SocketUp()); };
    auto Down = [&]() { Feed(FRedwoodSessionEvent::SocketDown()); };
    auto LogIn = [&]() {
      Feed(
        FRedwoodSessionEvent::Login(
          1, PlayerA, TEXT("secret"), TEXT("local"), false
        )
      );
    };
    auto LoginAccepted = [&]() {
      Feed(
        FRedwoodSessionEvent::LoginOk(
          S.GetJoinSeq(), PlayerA, TokenA, TEXT("Nick")
        )
      );
    };
    auto Enter = [&]() { Feed(FRedwoodSessionEvent::EnterRealm(2, RealmA)); };
    auto Entered = [&]() {
      Feed(
        FRedwoodSessionEvent::EnterRealmOk(
          S.GetStepSeq(), MakeShared<FJsonObject>()
        )
      );
    };
    auto Select = [&]() { Feed(FRedwoodSessionEvent::SelectCharacter(CharA)); };

    switch (Phase) {
      case EPhase::Connecting:
        break;
      case EPhase::ConnectFailed:
        Down();
        break;
      case EPhase::Online:
        Up();
        break;
      case EPhase::Offline:
        Up();
        Down();
        break;
      case EPhase::OfflineQuiet:
        Up();
        Feed(FRedwoodSessionEvent::SocketDown(false));
        break;
      case EPhase::LoggingIn:
        Up();
        LogIn();
        break;
      case EPhase::LoggedIn:
        Up();
        LogIn();
        LoginAccepted();
        break;
      case EPhase::EnteringRealm:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        break;
      case EPhase::InRealm:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        break;
      case EPhase::Suspended:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        break;
      case EPhase::Resuming:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        Up();
        break;
      case EPhase::Relogging:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        Up();
        Feed(FRedwoodSessionEvent::ResumeRefused());
        break;
      case EPhase::ReloginPending:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        Up();
        Feed(FRedwoodSessionEvent::ResumeRefused());
        Down();
        break;
      case EPhase::Reentering:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        Up();
        Feed(FRedwoodSessionEvent::ResumeRefused());
        LoginAccepted();
        break;
      case EPhase::LoggingOut:
        Up();
        LogIn();
        LoginAccepted();
        Enter();
        Entered();
        Select();
        Down();
        Up();
        Feed(FRedwoodSessionEvent::Logout());
        break;
      default:
        break;
    }
    return S;
  }

  // One expected effect: its kind, and the text and login answer when they
  // carry meaning. Sequences and request ids are checked by the scripts.
  struct FExpect {
    EKind Kind;
    FString Text;
    EAnswer Answer = EAnswer::Success;

    FExpect(EKind InKind) : Kind(InKind) {}
    FExpect(EKind InKind, const FString &InText) : Kind(InKind), Text(InText) {}
    FExpect(EKind InKind, EAnswer InAnswer, const FString &InText) :
      Kind(InKind), Text(InText), Answer(InAnswer) {}

    bool Matches(const FRedwoodSessionEffect &Effect) const {
      return Effect.Kind == Kind && Effect.Text == Text &&
        (Kind != EKind::AnswerLogin || Effect.LoginAnswer == Answer);
    }

    FString ToString() const {
      FString Result = FRedwoodClientSession::EffectName(Kind);
      if (!Text.IsEmpty()) {
        Result += TEXT(" \"") + Text + TEXT("\"");
      }
      return Result;
    }
  };

  // What ending a session emits, after any entry answer.
  TArray<FExpect> EndSession() {
    return {
      EKind::FailPendingReplies,
      EKind::ExpireHeld,
      EKind::StopGrace,
      EKind::ForgetSession,
      EKind::SessionCleared,
      EKind::RealmCleared,
    };
  }

  // A logout also forgets the remember-me credentials.
  TArray<FExpect> LoggedOutEnd() {
    TArray<FExpect> Effects = EndSession();
    Effects.Add(EKind::ClearCredentials);
    return Effects;
  }

  // The chat repair goes out before the held requests, so a held chat send
  // lands behind the lines the player missed.
  TArray<FExpect> Restored() {
    return {
      EKind::ResumeChat,
      EKind::ReleaseHeld,
      EKind::StopGrace,
      EKind::BroadcastReestablished
    };
  }

  TArray<FExpect> Cat(TArray<FExpect> A, const TArray<FExpect> &B) {
    A.Append(B);
    return A;
  }

  FExpect LoginError(const FString &Message) {
    return FExpect(EKind::AnswerLogin, EAnswer::Error, Message);
  }

  FExpect EntryAnswer(const FString &Error) {
    return FExpect(EKind::AnswerRealmEntry, Error);
  }

  struct FRow {
    EPhase From;
    EVariant Variant;
    EPhase To;
    TArray<FExpect> Effects;
  };

  const FString Lost = FRedwoodPendingReplies::LostReplyError;
  const FString NotConnected = FRedwoodClientSession::NotConnectedToDirector;
  const FString LoggedOut = FRedwoodClientSession::LoggedOutError;
  const FString Replaced = FRedwoodClientSession::ReplacedMessage;
  const FString NewerLogin = FRedwoodClientSession::NewerLoginError;
  const FString OtherRealm = FRedwoodClientSession::OtherRealmError;
  const FString EntryInProgress = FRedwoodClientSession::EntryInProgressError;
  const FString EntryNotReady = FRedwoodClientSession::EntryNotReadyError;

  // The transition table. A (phase, variant) with no row here must change
  // nothing. The order of effects is the order the adapter runs them.
  TArray<FRow> MakeTable() {
    TArray<FRow> T;
    const TArray<FExpect> Suspended = {
      EKind::StartGrace, EKind::FailPendingReplies, EKind::BroadcastLost
    };
    const TArray<FExpect> End = EndSession();
    const FExpect SelectA(EKind::SendSelectCharacter, CharA);

    // SocketUp
    T.Add(
      {EPhase::Connecting,
       EVariant::SocketUp,
       EPhase::Online,
       {EKind::ReportFirstConnect}}
    );
    T.Add(
      {EPhase::ConnectFailed,
       EVariant::SocketUp,
       EPhase::Online,
       {EKind::ReportFirstConnect, EKind::BroadcastReestablished}}
    );
    T.Add(
      {EPhase::Offline,
       EVariant::SocketUp,
       EPhase::Online,
       {EKind::BroadcastReestablished}}
    );
    T.Add({EPhase::OfflineQuiet, EVariant::SocketUp, EPhase::Online, {}});
    T.Add({EPhase::Suspended, EVariant::SocketUp, EPhase::Resuming, {}});
    T.Add(
      {EPhase::ReloginPending,
       EVariant::SocketUp,
       EPhase::Relogging,
       {EKind::SendLogin}}
    );

    // SocketDown, announced and quiet
    for (const EVariant Variant :
         {EVariant::SocketDownAnnounced, EVariant::SocketDownQuiet}) {
      const bool bQuiet = Variant == EVariant::SocketDownQuiet;
      T.Add(
        {EPhase::Connecting,
         Variant,
         EPhase::ConnectFailed,
         {EKind::LogFirstConnectFailure, EKind::BroadcastLost}}
      );
      if (bQuiet) {
        T.Add(
          {EPhase::Online,
           Variant,
           EPhase::OfflineQuiet,
           {EKind::FailPendingReplies}}
        );
      } else {
        T.Add(
          {EPhase::Online,
           Variant,
           EPhase::Offline,
           {EKind::FailPendingReplies, EKind::BroadcastLost}}
        );
      }
      T.Add(
        {EPhase::LoggingIn,
         Variant,
         EPhase::Offline,
         {EKind::FailPendingReplies, LoginError(Lost), EKind::BroadcastLost}}
      );
      T.Add({EPhase::LoggedIn, Variant, EPhase::Suspended, Suspended});
      T.Add(
        {EPhase::EnteringRealm,
         Variant,
         EPhase::Suspended,
         {EKind::StartGrace,
          EKind::FailPendingReplies,
          EntryAnswer(Lost),
          EKind::BroadcastLost}}
      );
      T.Add({EPhase::InRealm, Variant, EPhase::Suspended, Suspended});
      T.Add({EPhase::Resuming, Variant, EPhase::Suspended, {}});
      T.Add({EPhase::Relogging, Variant, EPhase::ReloginPending, {}});
      T.Add({EPhase::Reentering, Variant, EPhase::Suspended, {}});
      T.Add(
        {EPhase::LoggingOut, Variant, EPhase::Offline, {EKind::BroadcastLost}}
      );
    }

    // ResumeAccepted
    T.Add(
      {EPhase::Resuming,
       EVariant::ResumeAcceptedRealm,
       EPhase::InRealm,
       Cat({EKind::RealmEntered}, Restored())}
    );
    T.Add(
      {EPhase::Resuming,
       EVariant::ResumeAcceptedOtherCharacter,
       EPhase::InRealm,
       Cat({EKind::RealmEntered, SelectA}, Restored())}
    );
    T.Add(
      {EPhase::Resuming,
       EVariant::ResumeAcceptedLocked,
       EPhase::Reentering,
       {FExpect(EKind::SendEnterRealm, RealmA)}}
    );
    T.Add(
      {EPhase::Resuming,
       EVariant::ResumeAcceptedNone,
       EPhase::Reentering,
       {FExpect(EKind::SendEnterRealm, RealmA)}}
    );
    for (const EVariant Variant :
         {EVariant::ResumeAcceptedRealm,
          EVariant::ResumeAcceptedOtherCharacter,
          EVariant::ResumeAcceptedLocked,
          EVariant::ResumeAcceptedNone}) {
      T.Add(
        {EPhase::LoggingOut,
         Variant,
         EPhase::Online,
         {EKind::SendLogoutStep, EKind::ForgetSession}}
      );
    }

    // ResumeRefused
    T.Add(
      {EPhase::Resuming,
       EVariant::ResumeRefused,
       EPhase::Relogging,
       {EKind::SendLogin}}
    );
    T.Add({EPhase::LoggingOut, EVariant::ResumeRefused, EPhase::Online, {}});

    // ClosedByServer: replaced ends the session; reconnect changes nothing.
    const FExpect AuthReplaced(EKind::BroadcastAuthFailed, Replaced);
    for (const EPhase From :
         {EPhase::LoggedIn, EPhase::InRealm, EPhase::Reentering}) {
      T.Add(
        {From,
         EVariant::ClosedReplaced,
         EPhase::Online,
         Cat(End, {AuthReplaced})}
      );
    }
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::ClosedReplaced,
       EPhase::Online,
       Cat(Cat({EntryAnswer(Replaced)}, End), {AuthReplaced})}
    );

    // LoginReply
    T.Add(
      {EPhase::LoggingIn,
       EVariant::LoginOk,
       EPhase::LoggedIn,
       {EKind::ClearCredentials,
        FExpect(EKind::AnswerLogin, EAnswer::Success, FString())}}
    );
    T.Add(
      {EPhase::LoggingIn,
       EVariant::LoginRefused,
       EPhase::Online,
       {EKind::ClearCredentials, LoginError(Refusal)}}
    );
    T.Add(
      {EPhase::LoggingIn,
       EVariant::LoginMustVerify,
       EPhase::Online,
       {FExpect(EKind::AnswerLogin, EAnswer::MustVerify, FString())}}
    );
    T.Add(
      {EPhase::LoggingIn,
       EVariant::LoginLost,
       EPhase::Online,
       {LoginError(Lost)}}
    );
    T.Add(
      {EPhase::Relogging,
       EVariant::LoginOk,
       EPhase::Reentering,
       {EKind::SaveCredentials, FExpect(EKind::SendEnterRealm, RealmA)}}
    );
    T.Add(
      {EPhase::Relogging,
       EVariant::LoginRefused,
       EPhase::Online,
       Cat(End, {FExpect(EKind::BroadcastAuthFailed, Refusal)})}
    );
    T.Add(
      {EPhase::Relogging,
       EVariant::LoginMustVerify,
       EPhase::Online,
       Cat(
         End, {FExpect(EKind::BroadcastAuthFailed, TEXT("Must verify account"))}
       )}
    );
    T.Add({EPhase::Relogging, EVariant::LoginLost, EPhase::ReloginPending, {}});
    for (const EVariant Variant : {EVariant::LoginOk}) {
      T.Add(
        {EPhase::LoggingOut,
         Variant,
         EPhase::Online,
         {EKind::SendLogoutStep, EKind::ForgetSession}}
      );
    }
    for (const EVariant Variant :
         {EVariant::LoginRefused,
          EVariant::LoginMustVerify,
          EVariant::LoginLost}) {
      T.Add({EPhase::LoggingOut, Variant, EPhase::Online, {}});
    }

    // EnterRealmReply
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::EnterOk,
       EPhase::InRealm,
       {EKind::ApplyRegions, EKind::RealmEntered, EntryAnswer(FString())}}
    );
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::EnterRefused,
       EPhase::LoggedIn,
       {EKind::RealmCleared, EntryAnswer(RealmRefusal)}}
    );
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::EnterLost,
       EPhase::Suspended,
       {EKind::StartGrace,
        EKind::FailPendingReplies,
        EntryAnswer(Lost),
        EKind::BroadcastLost}}
    );
    T.Add(
      {EPhase::Reentering,
       EVariant::EnterOk,
       EPhase::InRealm,
       Cat({EKind::ApplyRegions, EKind::RealmEntered, SelectA}, Restored())}
    );
    T.Add(
      {EPhase::Reentering,
       EVariant::EnterRefused,
       EPhase::Online,
       Cat(
         Cat({EKind::SendLogoutStep}, End),
         {FExpect(EKind::BroadcastAuthFailed, RealmRefusal)}
       )}
    );
    T.Add({EPhase::Reentering, EVariant::EnterLost, EPhase::Suspended, {}});

    // GraceExpired
    for (const EPhase From :
         {EPhase::Suspended,
          EPhase::ReloginPending,
          EPhase::Resuming,
          EPhase::Relogging,
          EPhase::Reentering}) {
      T.Add({From, EVariant::GraceExpired, From, {EKind::ExpireHeld}});
    }

    // Initialize: a new connection; whatever was there goes, with no word.
    for (const EPhase From :
         {EPhase::Connecting,
          EPhase::ConnectFailed,
          EPhase::Online,
          EPhase::Offline,
          EPhase::OfflineQuiet}) {
      T.Add(
        {From,
         EVariant::Initialize,
         FRedwoodClientSession::IsOutagePhase(From) ? EPhase::ConnectFailed
                                                    : EPhase::Connecting,
         {}}
      );
    }
    T.Add(
      {EPhase::LoggingIn,
       EVariant::Initialize,
       EPhase::Connecting,
       Cat(End, {LoginError(NotConnected)})}
    );
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::Initialize,
       EPhase::Connecting,
       Cat({EntryAnswer(EntryNotReady)}, End)}
    );
    for (const EPhase From :
         {EPhase::LoggedIn,
          EPhase::InRealm,
          EPhase::Suspended,
          EPhase::ReloginPending,
          EPhase::Resuming,
          EPhase::Relogging,
          EPhase::Reentering,
          EPhase::LoggingOut}) {
      // During an outage the game heard a loss; the new socket's first
      // connect must also reestablish it.
      T.Add(
        {From,
         EVariant::Initialize,
         FRedwoodClientSession::IsOutagePhase(From) ? EPhase::ConnectFailed
                                                    : EPhase::Connecting,
         End}
      );
    }

    // Login: starts clean, from anywhere a socket can carry it.
    for (const EPhase From :
         {EPhase::Connecting,
          EPhase::ConnectFailed,
          EPhase::Offline,
          EPhase::OfflineQuiet}) {
      T.Add({From, EVariant::Login, From, {LoginError(NotConnected)}});
    }
    for (const EPhase From : {EPhase::Suspended, EPhase::ReloginPending}) {
      T.Add({From, EVariant::Login, From, {LoginError(NotConnected)}});
    }
    T.Add(
      {EPhase::Online,
       EVariant::Login,
       EPhase::LoggingIn,
       {EKind::RealmCleared, EKind::SendLogin}}
    );
    T.Add(
      {EPhase::LoggingIn,
       EVariant::Login,
       EPhase::LoggingIn,
       Cat(Cat(End, {LoginError(NewerLogin)}), {EKind::SendLogin})}
    );
    for (const EPhase From :
         {EPhase::Resuming, EPhase::Relogging, EPhase::LoggingOut}) {
      T.Add(
        {From, EVariant::Login, EPhase::LoggingIn, Cat(End, {EKind::SendLogin})}
      );
    }
    T.Add(
      {EPhase::InRealm,
       EVariant::Login,
       EPhase::LoggingIn,
       Cat(
         Cat(
           {FExpect(EKind::PayOwedLeave, PlayerA), EKind::SendLogoutStep}, End
         ),
         {EKind::SendLogin}
       )}
    );
    for (const EPhase From : {EPhase::LoggedIn, EPhase::Reentering}) {
      T.Add(
        {From,
         EVariant::Login,
         EPhase::LoggingIn,
         Cat(Cat({EKind::SendLogoutStep}, End), {EKind::SendLogin})}
      );
    }
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::Login,
       EPhase::LoggingIn,
       Cat(
         Cat({EKind::SendLogoutStep, EntryAnswer(NewerLogin)}, End),
         {EKind::SendLogin}
       )}
    );

    // Logout
    const TArray<FExpect> Left = LoggedOutEnd();
    T.Add(
      {EPhase::LoggingIn,
       EVariant::Logout,
       EPhase::LoggingOut,
       Cat(Left, {LoginError(LoggedOut)})}
    );
    T.Add(
      {EPhase::LoggedIn,
       EVariant::Logout,
       EPhase::Online,
       Cat({EKind::SendLogoutStep}, Left)}
    );
    T.Add(
      {EPhase::EnteringRealm,
       EVariant::Logout,
       EPhase::Online,
       Cat({EntryAnswer(LoggedOut), EKind::SendLogoutStep}, Left)}
    );
    T.Add(
      {EPhase::InRealm,
       EVariant::Logout,
       EPhase::Online,
       Cat(
         {FExpect(EKind::PayOwedLeave, PlayerA), EKind::SendLogoutStep}, Left
       )}
    );
    for (const EPhase From : {EPhase::Suspended, EPhase::ReloginPending}) {
      T.Add({From, EVariant::Logout, EPhase::Offline, Left});
    }
    for (const EPhase From : {EPhase::Resuming, EPhase::Relogging}) {
      T.Add({From, EVariant::Logout, EPhase::LoggingOut, Left});
    }
    T.Add(
      {EPhase::Reentering,
       EVariant::Logout,
       EPhase::Online,
       Cat({EKind::SendLogoutStep}, Left)}
    );

    // EnterRealm
    T.Add(
      {EPhase::LoggedIn,
       EVariant::EnterRealmSame,
       EPhase::EnteringRealm,
       {EKind::RealmCleared, FExpect(EKind::SendEnterRealm, RealmA)}}
    );
    T.Add(
      {EPhase::LoggedIn,
       EVariant::EnterRealmOther,
       EPhase::EnteringRealm,
       {EKind::RealmCleared, FExpect(EKind::SendEnterRealm, RealmB)}}
    );
    T.Add(
      {EPhase::InRealm,
       EVariant::EnterRealmSame,
       EPhase::InRealm,
       {EntryAnswer(FString())}}
    );
    T.Add(
      {EPhase::InRealm,
       EVariant::EnterRealmOther,
       EPhase::InRealm,
       {EntryAnswer(OtherRealm)}}
    );
    for (const EPhase From : {EPhase::EnteringRealm, EPhase::Reentering}) {
      T.Add({From, EVariant::EnterRealmSame, From, {}});
      T.Add(
        {From, EVariant::EnterRealmOther, From, {EntryAnswer(EntryInProgress)}}
      );
    }
    for (const EPhase From :
         {EPhase::Connecting,
          EPhase::ConnectFailed,
          EPhase::Online,
          EPhase::Offline,
          EPhase::OfflineQuiet,
          EPhase::LoggingIn,
          EPhase::Suspended,
          EPhase::ReloginPending,
          EPhase::Resuming,
          EPhase::Relogging,
          EPhase::LoggingOut}) {
      T.Add(
        {From, EVariant::EnterRealmSame, From, {EntryAnswer(EntryNotReady)}}
      );
      T.Add(
        {From, EVariant::EnterRealmOther, From, {EntryAnswer(EntryNotReady)}}
      );
    }

    // SelectCharacter
    T.Add(
      {EPhase::InRealm,
       EVariant::SelectCharacter,
       EPhase::InRealm,
       {FExpect(EKind::SendSelectCharacter, CharB)}}
    );

    return T;
  }

  const FRow *FindRow(
    const TArray<FRow> &Table, EPhase From, EVariant Variant
  ) {
    for (const FRow &Row : Table) {
      if (Row.From == From && Row.Variant == Variant) {
        return &Row;
      }
    }
    return nullptr;
  }

  FString Describe(const TArray<FRedwoodSessionEffect> &Effects) {
    FString Result;
    for (const FRedwoodSessionEffect &Effect : Effects) {
      Result += (Result.IsEmpty() ? TEXT("") : TEXT(", ")) + Effect.ToString();
    }
    return Result.IsEmpty() ? TEXT("(none)") : Result;
  }

  FString Describe(const TArray<FExpect> &Effects) {
    FString Result;
    for (const FExpect &Effect : Effects) {
      Result += (Result.IsEmpty() ? TEXT("") : TEXT(", ")) + Effect.ToString();
    }
    return Result.IsEmpty() ? TEXT("(none)") : Result;
  }

  bool SameEffects(
    const TArray<FExpect> &Expected, const TArray<FRedwoodSessionEffect> &Actual
  ) {
    if (Expected.Num() != Actual.Num()) {
      return false;
    }
    for (int32 Index = 0; Index < Expected.Num(); ++Index) {
      if (!Expected[Index].Matches(Actual[Index])) {
        return false;
      }
    }
    return true;
  }

  int32 Count(const TArray<FRedwoodSessionEffect> &Effects, EKind Kind) {
    int32 Result = 0;
    for (const FRedwoodSessionEffect &Effect : Effects) {
      if (Effect.Kind == Kind) {
        ++Result;
      }
    }
    return Result;
  }

  // Runs a script of events and keeps every effect, for the scenario tests.
  struct FScript {
    FRedwoodClientSession S;
    TArray<FRedwoodSessionEffect> Log;

    void Feed(const FRedwoodSessionEvent &Event) {
      Log.Append(S.Handle(Event));
    }
    void Up() {
      Feed(FRedwoodSessionEvent::SocketUp());
    }
    void Down() {
      Feed(FRedwoodSessionEvent::SocketDown());
    }
    void LogIn(uint32 Request = 1) {
      Feed(
        FRedwoodSessionEvent::Login(
          Request, PlayerA, TEXT("secret"), TEXT("local"), false
        )
      );
    }
    void LoginAccepted(const FString &Token = TokenA) {
      Feed(
        FRedwoodSessionEvent::LoginOk(
          S.GetJoinSeq(), PlayerA, Token, TEXT("Nick")
        )
      );
    }
    void Enter(uint32 Request = 2, const FString &Realm = RealmA) {
      Feed(FRedwoodSessionEvent::EnterRealm(Request, Realm));
    }
    void Entered() {
      Feed(
        FRedwoodSessionEvent::EnterRealmOk(
          S.GetStepSeq(), MakeShared<FJsonObject>()
        )
      );
    }
    void Select(const FString &Character = CharA) {
      Feed(FRedwoodSessionEvent::SelectCharacter(Character));
    }
    // In the realm with a character, the way a player in the world is.
    void InWorld() {
      Up();
      LogIn();
      LoginAccepted();
      Enter();
      Entered();
      Select();
      Log.Reset();
    }
    int32 Lost() const {
      return Count(Log, EKind::BroadcastLost);
    }
    int32 Reestablished() const {
      return Count(Log, EKind::BroadcastReestablished);
    }
    int32 AuthFailed() const {
      return Count(Log, EKind::BroadcastAuthFailed);
    }
    const FRedwoodSessionEffect *Last(EKind Kind) const {
      for (int32 Index = Log.Num() - 1; Index >= 0; --Index) {
        if (Log[Index].Kind == Kind) {
          return &Log[Index];
        }
      }
      return nullptr;
    }
  };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSessionMachineTableTest,
  "Redwood.Session.Machine.Table",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSessionMachineTableTest::RunTest(const FString &Parameters) {
  using namespace RedwoodSessionMachineTest;
  // A socket that comes up in a phase that has one is a warning the machine
  // keeps, so a future adapter bug is loud.
  AddExpectedMessage(
    TEXT("which has one already"),
    ELogVerbosity::Warning,
    EAutomationExpectedMessageFlags::Contains,
    0
  );

  const TArray<FRow> Table = MakeTable();

  // Every phase is reachable by its canonical path.
  for (int32 P = 0; P < static_cast<int32>(EPhase::Count); ++P) {
    const EPhase Phase = static_cast<EPhase>(P);
    const FRedwoodClientSession S = MakeIn(Phase);
    TestEqual(
      FString::Printf(
        TEXT("MakeIn reaches %s"), FRedwoodClientSession::PhaseName(Phase)
      ),
      FRedwoodClientSession::PhaseName(S.GetPhase()),
      FRedwoodClientSession::PhaseName(Phase)
    );
  }

  int32 Cells = 0;
  for (int32 P = 0; P < static_cast<int32>(EPhase::Count); ++P) {
    for (int32 V = 0; V < static_cast<int32>(EVariant::Count); ++V) {
      const EPhase From = static_cast<EPhase>(P);
      const EVariant Variant = static_cast<EVariant>(V);
      FRedwoodClientSession S = MakeIn(From);
      const TArray<FRedwoodSessionEffect> Effects =
        S.Handle(MakeEvent(Variant, S));
      const FString Cell = FString::Printf(
        TEXT("%s + %s"),
        FRedwoodClientSession::PhaseName(From),
        VariantName(Variant)
      );
      ++Cells;

      const FRow *Row = FindRow(Table, From, Variant);
      const EPhase ExpectedTo = Row ? Row->To : From;
      const TArray<FExpect> ExpectedEffects =
        Row ? Row->Effects : TArray<FExpect>();
      if (S.GetPhase() != ExpectedTo) {
        AddError(
          FString::Printf(
            TEXT("%s: expected phase %s, got %s"),
            *Cell,
            FRedwoodClientSession::PhaseName(ExpectedTo),
            FRedwoodClientSession::PhaseName(S.GetPhase())
          )
        );
      }
      if (!SameEffects(ExpectedEffects, Effects)) {
        AddError(
          FString::Printf(
            TEXT("%s: expected effects [%s], got [%s]"),
            *Cell,
            *Describe(ExpectedEffects),
            *Describe(Effects)
          )
        );
      }
    }
  }
  TestEqual(
    TEXT("Every cell was walked"),
    Cells,
    static_cast<int32>(EPhase::Count) * static_cast<int32>(EVariant::Count)
  );
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSessionMachineInvariantsTest,
  "Redwood.Session.Machine.Invariants",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSessionMachineInvariantsTest::RunTest(const FString &Parameters) {
  using namespace RedwoodSessionMachineTest;
  AddExpectedMessage(
    TEXT("which has one already"),
    ELogVerbosity::Warning,
    EAutomationExpectedMessageFlags::Contains,
    0
  );

  for (int32 P = 0; P < static_cast<int32>(EPhase::Count); ++P) {
    for (int32 V = 0; V < static_cast<int32>(EVariant::Count); ++V) {
      const EPhase From = static_cast<EPhase>(P);
      const EVariant Variant = static_cast<EVariant>(V);
      FRedwoodClientSession S = MakeIn(From);
      const FRedwoodSessionEvent Event = MakeEvent(Variant, S);
      const TArray<FRedwoodSessionEffect> Effects = S.Handle(Event);
      const EPhase To = S.GetPhase();
      const FString Cell = FString::Printf(
        TEXT("%s + %s -> %s"),
        FRedwoodClientSession::PhaseName(From),
        VariantName(Variant),
        FRedwoodClientSession::PhaseName(To)
      );

      const bool bWasOutage = FRedwoodClientSession::IsOutagePhase(From);
      const bool bIsOutage = FRedwoodClientSession::IsOutagePhase(To);
      const int32 LostCount = Count(Effects, EKind::BroadcastLost);
      const int32 BackCount = Count(Effects, EKind::BroadcastReestablished);
      const int32 FailCount = Count(Effects, EKind::BroadcastAuthFailed);

      // A lost connection is broadcast exactly when an outage starts.
      TestEqual(
        Cell + TEXT(": Lost iff an outage starts"),
        LostCount,
        (!bWasOutage && bIsOutage) ? 1 : 0
      );

      // An outage ends with one word to the game, or by the player's hand.
      const bool bPlayersOwn = Event.Kind == ERedwoodSessionEventKind::Logout ||
        Event.Kind == ERedwoodSessionEventKind::Login ||
        Event.Kind == ERedwoodSessionEventKind::Initialize;
      if (bWasOutage && !bIsOutage) {
        TestTrue(
          Cell +
            TEXT(
              ": an outage ends with Reestablished, AuthFailed, or the player's own call"
            ),
          BackCount + FailCount == 1 ||
            (bPlayersOwn && BackCount + FailCount == 0)
        );
      } else {
        TestEqual(
          Cell + TEXT(": no Reestablished outside the end of an outage"),
          BackCount,
          0
        );
      }

      // An authentication failure only ends a session, and only for Online.
      if (FailCount > 0) {
        TestTrue(
          Cell + TEXT(": AuthFailed leaves a session phase"),
          FRedwoodClientSession::IsLoggedInPhase(From) ||
            FRedwoodClientSession::IsComingBackPhase(From)
        );
        TestTrue(
          Cell + TEXT(": AuthFailed lands in Online"), To == EPhase::Online
        );
        TestEqual(Cell + TEXT(": AuthFailed once"), FailCount, 1);
      }

      // Never two of the three in one row.
      TestTrue(
        Cell + TEXT(": at most one word to the game"),
        LostCount + BackCount + FailCount <= 1
      );

      // The grace flag lives only while the session is coming back.
      if (!S.IsComingBack()) {
        TestFalse(
          Cell + TEXT(": no grace flag outside the coming-back phases"),
          S.IsGraceExpired()
        );
      }

      // A session that ends is forgotten on the connection too; one that
      // continues keeps its ids and is not forgotten.
      const bool bHadSession = FRedwoodClientSession::IsLoggedInPhase(From) ||
        FRedwoodClientSession::IsComingBackPhase(From);
      const bool bHasSession = S.HasPlayerSession();
      if (bHadSession && bHasSession) {
        TestEqual(
          Cell + TEXT(": a session that continues is not forgotten"),
          Count(Effects, EKind::ForgetSession),
          0
        );
        TestEqual(
          Cell + TEXT(": a session that continues keeps its player"),
          S.GetPlayerId(),
          PlayerA
        );
      }
      if (bHadSession && !bHasSession) {
        TestTrue(
          Cell + TEXT(": a session that ends forgets its ids"),
          S.GetPlayerId().IsEmpty() && S.GetAuthToken().IsEmpty()
        );
        TestEqual(
          Cell + TEXT(": a session that ends is forgotten on the connection"),
          Count(Effects, EKind::ForgetSession),
          1
        );
      }
    }
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSessionMachineGateTest,
  "Redwood.Session.Machine.Gate",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSessionMachineGateTest::RunTest(const FString &Parameters) {
  using namespace RedwoodSessionMachineTest;
  using EGate = ERedwoodGateDecision;
  using EReq = ERedwoodRequestKind;

  for (int32 P = 0; P < static_cast<int32>(EPhase::Count); ++P) {
    const EPhase Phase = static_cast<EPhase>(P);
    const TCHAR *Name = FRedwoodClientSession::PhaseName(Phase);
    FRedwoodClientSession S = MakeIn(Phase);

    EGate Director = EGate::Fail;
    EGate Realm = EGate::Fail;
    switch (Phase) {
      case EPhase::LoggedIn:
      case EPhase::EnteringRealm:
        Director = EGate::Send;
        break;
      case EPhase::InRealm:
        Director = EGate::Send;
        Realm = EGate::Send;
        break;
      case EPhase::Suspended:
      case EPhase::ReloginPending:
      case EPhase::Resuming:
      case EPhase::Relogging:
      case EPhase::Reentering:
        // The canonical path dropped from the realm, so there is one to go
        // back to.
        Director = EGate::Hold;
        Realm = EGate::Hold;
        break;
      default:
        break;
    }
    TestEqual(
      FString::Printf(TEXT("%s: director gate"), Name),
      static_cast<int32>(S.Gate(EReq::Director)),
      static_cast<int32>(Director)
    );
    TestEqual(
      FString::Printf(TEXT("%s: realm gate"), Name),
      static_cast<int32>(S.Gate(EReq::Realm)),
      static_cast<int32>(Realm)
    );

    // Once the grace ran out, a held phase fails instead.
    if (FRedwoodClientSession::IsComingBackPhase(Phase)) {
      S.Handle(FRedwoodSessionEvent::GraceExpired());
      TestEqual(
        FString::Printf(TEXT("%s: director gate after the grace"), Name),
        static_cast<int32>(S.Gate(EReq::Director)),
        static_cast<int32>(EGate::Fail)
      );
      TestEqual(
        FString::Printf(TEXT("%s: realm gate after the grace"), Name),
        static_cast<int32>(S.Gate(EReq::Realm)),
        static_cast<int32>(EGate::Fail)
      );
    }
  }

  // A drop from LoggedIn (no realm) holds director requests and fails realm
  // ones: there is no realm to come back to.
  {
    FRedwoodClientSession S = MakeIn(EPhase::LoggedIn);
    S.Handle(FRedwoodSessionEvent::SocketDown());
    TestEqual(
      TEXT("Suspended with no realm: director held"),
      static_cast<int32>(S.Gate(EReq::Director)),
      static_cast<int32>(EGate::Hold)
    );
    TestEqual(
      TEXT("Suspended with no realm: realm fails"),
      static_cast<int32>(S.Gate(EReq::Realm)),
      static_cast<int32>(EGate::Fail)
    );
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSessionMachineQueriesTest,
  "Redwood.Session.Machine.Queries",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSessionMachineQueriesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodSessionMachineTest;

  struct FExpectQueries {
    EPhase Phase;
    bool bSocketUp;
    bool bLoggedIn;
    bool bHasSession;
    bool bRealmEntered;
    bool bOutage;
    bool bComingBack;
    FString RealmId;
  };
  const TArray<FExpectQueries> Expected = {
    {EPhase::Connecting, false, false, false, false, false, false, FString()},
    {EPhase::ConnectFailed, false, false, false, false, true, false, FString()},
    {EPhase::Online, true, false, false, false, false, false, FString()},
    {EPhase::Offline, false, false, false, false, true, false, FString()},
    {EPhase::OfflineQuiet, false, false, false, false, false, false, FString()},
    {EPhase::LoggingIn, true, false, false, false, false, false, FString()},
    {EPhase::LoggedIn, true, true, true, false, false, false, FString()},
    {EPhase::EnteringRealm, true, true, true, false, false, false, FString()},
    {EPhase::InRealm, true, true, true, true, false, false, RealmA},
    {EPhase::Suspended, false, false, true, false, true, true, RealmA},
    {EPhase::ReloginPending, false, false, true, false, true, true, RealmA},
    {EPhase::Resuming, true, false, true, false, true, true, RealmA},
    {EPhase::Relogging, true, false, true, false, true, true, RealmA},
    {EPhase::Reentering, true, false, true, false, true, true, RealmA},
    {EPhase::LoggingOut, true, false, false, false, false, false, FString()},
  };
  TestEqual(
    TEXT("Every phase has a row"),
    Expected.Num(),
    static_cast<int32>(EPhase::Count)
  );

  for (const FExpectQueries &Row : Expected) {
    const FRedwoodClientSession S = MakeIn(Row.Phase);
    const FString Name = FRedwoodClientSession::PhaseName(Row.Phase);
    TestEqual(Name + TEXT(": IsSocketUp"), S.IsSocketUp(), Row.bSocketUp);
    TestEqual(Name + TEXT(": IsLoggedIn"), S.IsLoggedIn(), Row.bLoggedIn);
    TestEqual(
      Name + TEXT(": HasPlayerSession"), S.HasPlayerSession(), Row.bHasSession
    );
    TestEqual(
      Name + TEXT(": IsRealmEntered"), S.IsRealmEntered(), Row.bRealmEntered
    );
    TestEqual(Name + TEXT(": IsOutage"), S.IsOutage(), Row.bOutage);
    TestEqual(Name + TEXT(": IsComingBack"), S.IsComingBack(), Row.bComingBack);
    TestEqual(Name + TEXT(": GetRealmId"), S.GetRealmId(), Row.RealmId);
  }

  // The ids live with the session.
  {
    const FRedwoodClientSession S = MakeIn(EPhase::InRealm);
    TestEqual(TEXT("InRealm: player id"), S.GetPlayerId(), PlayerA);
    TestEqual(TEXT("InRealm: token"), S.GetAuthToken(), TokenA);
    TestEqual(TEXT("InRealm: nickname"), S.GetNickname(), TEXT("Nick"));
    TestEqual(TEXT("InRealm: character"), S.GetSelectedCharacterId(), CharA);
  }
  {
    FRedwoodClientSession S = MakeIn(EPhase::InRealm);
    S.Handle(FRedwoodSessionEvent::Logout());
    TestTrue(TEXT("After a logout: no player id"), S.GetPlayerId().IsEmpty());
    TestTrue(TEXT("After a logout: no token"), S.GetAuthToken().IsEmpty());
    TestTrue(
      TEXT("After a logout: no character"), S.GetSelectedCharacterId().IsEmpty()
    );
    TestTrue(TEXT("After a logout: no realm"), S.GetRealmId().IsEmpty());
  }
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSessionMachineHolesTest,
  "Redwood.Session.Machine.Holes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSessionMachineHolesTest::RunTest(const FString &Parameters) {
  using namespace RedwoodSessionMachineTest;

  // 1. A drop during the re-login does not end the session: the next socket
  //    logs in again, and the whole outage is one Lost and one Reestablished.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeRefused());
    T.Feed(FRedwoodSessionEvent::LoginLost(T.S.GetJoinSeq()));
    TestEqual(
      TEXT("1: waits for a socket"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::ReloginPending)
    );
    TestEqual(TEXT("1: no AuthFailed"), T.AuthFailed(), 0);
    T.Up();
    TestEqual(TEXT("1: logs in again"), Count(T.Log, EKind::SendLogin), 2);
    T.LoginAccepted(TokenB);
    T.Entered();
    TestEqual(
      TEXT("1: back in the realm"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::InRealm)
    );
    TestEqual(TEXT("1: one Lost"), T.Lost(), 1);
    TestEqual(TEXT("1: one Reestablished"), T.Reestablished(), 1);
    TestEqual(
      TEXT("1: the character is selected again"),
      Count(T.Log, EKind::SendSelectCharacter),
      1
    );
  }

  // 2. A drop during the realm re-entry keeps the realm: the next socket
  //    resumes the new session, and Bifrost names the realm it is locked to.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeRefused());
    T.LoginAccepted(TokenB);
    T.Feed(FRedwoodSessionEvent::EnterRealmLost(T.S.GetStepSeq()));
    TestEqual(
      TEXT("2: suspended"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::Suspended)
    );
    TestEqual(TEXT("2: no AuthFailed"), T.AuthFailed(), 0);
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(FString(), RealmA, FString()));
    const FRedwoodSessionEffect *Enter = T.Last(EKind::SendEnterRealm);
    TestTrue(
      TEXT("2: enters the locked realm"), Enter && Enter->Text == RealmA
    );
    T.Entered();
    TestEqual(TEXT("2: one Lost"), T.Lost(), 1);
    TestEqual(TEXT("2: one Reestablished"), T.Reestablished(), 1);
  }

  // 3. A refused re-login ends the session for good: no socket logs in with
  //    the dead token again.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeRefused());
    T.Feed(
      FRedwoodSessionEvent::LoginRefused(
        T.S.GetJoinSeq(), TEXT("login_failed"), TEXT("Token expired")
      )
    );
    TestEqual(TEXT("3: one AuthFailed"), T.AuthFailed(), 1);
    const FRedwoodSessionEffect *Failed = T.Last(EKind::BroadcastAuthFailed);
    TestTrue(
      TEXT("3: with the text"), Failed && Failed->Text == TEXT("Token expired")
    );
    TestFalse(TEXT("3: no session"), T.S.HasPlayerSession());
    T.Down();
    T.Up();
    TestEqual(TEXT("3: no login"), Count(T.Log, EKind::SendLogin), 1);
    TestEqual(
      TEXT("3: the later drop is a new loss at the title"), T.Lost(), 2
    );
  }

  // 4. The first drop after a healthy first connect is a loss.
  {
    FScript T;
    T.InWorld();
    T.Down();
    TestEqual(TEXT("4: Lost"), T.Lost(), 1);
    TestEqual(
      TEXT("4: requests held"),
      static_cast<int32>(T.S.Gate(ERedwoodRequestKind::Realm)),
      static_cast<int32>(ERedwoodGateDecision::Hold)
    );
  }

  // 5. A logout while the resume is in flight ends the session the resume
  //    brings back, with no reestablished connection.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::Logout());
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestEqual(TEXT("5: logout step"), Count(T.Log, EKind::SendLogoutStep), 1);
    TestEqual(
      TEXT("5: session forgotten"), Count(T.Log, EKind::ForgetSession), 2
    );
    TestEqual(TEXT("5: no Reestablished"), T.Reestablished(), 0);
    TestEqual(
      TEXT("5: online"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::Online)
    );
  }

  // 6. A login right after that logout starts clean, and the old join's
  //    late reply cannot act.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::Logout());
    const uint32 OldJoin = T.S.GetJoinSeq();
    T.LogIn(5);
    T.Feed(
      FRedwoodSessionEvent::LoginOk(OldJoin, PlayerA, TokenA, TEXT("Old"))
    );
    TestEqual(
      TEXT("6: still logging in"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::LoggingIn)
    );
    T.LoginAccepted(TokenB);
    TestEqual(
      TEXT("6: logged in with the new token"), T.S.GetAuthToken(), TokenB
    );
    TestTrue(TEXT("6: in no realm"), T.S.GetRealmId().IsEmpty());
    const FRedwoodSessionEffect *Answer = T.Last(EKind::AnswerLogin);
    TestTrue(
      TEXT("6: the new login is answered"), Answer && Answer->RequestId == 5
    );
  }

  // 7. A resume the pod refuses for its own trouble drops the socket again:
  //    the same outage, no second Lost.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestEqual(TEXT("7: one Lost"), T.Lost(), 1);
    TestEqual(TEXT("7: one Reestablished"), T.Reestablished(), 1);
  }

  // 8. The grace: held, then failed, then sent again when the session is
  //    back.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Feed(FRedwoodSessionEvent::GraceExpired());
    TestEqual(
      TEXT("8: fails after the grace"),
      static_cast<int32>(T.S.Gate(ERedwoodRequestKind::Realm)),
      static_cast<int32>(ERedwoodGateDecision::Fail)
    );
    TestEqual(
      TEXT("8: the held requests expired"), Count(T.Log, EKind::ExpireHeld), 1
    );
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestEqual(
      TEXT("8: sends again"),
      static_cast<int32>(T.S.Gate(ERedwoodRequestKind::Realm)),
      static_cast<int32>(ERedwoodGateDecision::Send)
    );
  }

  // 9. A logout during the drop; the socket that comes back has nothing to
  //    bring, and the outage ends with one Reestablished.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Feed(FRedwoodSessionEvent::Logout());
    TestEqual(TEXT("9: nothing sent"), Count(T.Log, EKind::SendLogoutStep), 0);
    T.Up();
    TestEqual(TEXT("9: no join"), Count(T.Log, EKind::SendLogin), 0);
    TestEqual(TEXT("9: one Reestablished"), T.Reestablished(), 1);
    TestEqual(
      TEXT("9: online"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::Online)
    );
  }

  // 10. The entry lost with the socket before the client learned the realm:
  //     the game's caller is answered, Bifrost names the realm, and the
  //     game's retry for the same realm is answered at once.
  {
    FScript T;
    T.Up();
    T.LogIn();
    T.LoginAccepted();
    T.Enter(2);
    T.Down();
    const FRedwoodSessionEffect *Lost1 = T.Last(EKind::AnswerRealmEntry);
    TestTrue(
      TEXT("10: the entry is answered as lost"),
      Lost1 && Lost1->RequestId == 2 && Lost1->Text == Lost
    );
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(FString(), RealmA, FString()));
    T.Entered();
    TestTrue(
      TEXT("10: in the realm Bifrost named"),
      T.S.IsRealmEntered() && T.S.GetRealmId() == RealmA
    );
    T.Enter(3);
    const FRedwoodSessionEffect *Again = T.Last(EKind::AnswerRealmEntry);
    TestTrue(
      TEXT("10: the retry is answered at once"),
      Again && Again->RequestId == 3 && Again->Text.IsEmpty()
    );
    TestEqual(
      TEXT("10: one entry step for the retry path"),
      Count(T.Log, EKind::SendEnterRealm),
      2
    );
  }

  // 11. A session replaced by another login ends once, and the socket that
  //     Bifrost then closes is no loss.
  {
    FScript T;
    T.InWorld();
    T.Feed(FRedwoodSessionEvent::ClosedByServer(TEXT("replaced")));
    T.Feed(FRedwoodSessionEvent::SocketDown(false));
    T.Up();
    TestEqual(TEXT("11: one AuthFailed"), T.AuthFailed(), 1);
    TestEqual(TEXT("11: no Lost"), T.Lost(), 0);
    TestEqual(TEXT("11: no Reestablished"), T.Reestablished(), 0);
    TestEqual(
      TEXT("11: online"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::Online)
    );
  }

  // 12. A realm re-entry the realm refuses ends the session, and the new
  //     login is logged out of Bifrost.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeRefused());
    T.LoginAccepted(TokenB);
    T.Feed(
      FRedwoodSessionEvent::EnterRealmRefused(T.S.GetStepSeq(), RealmRefusal)
    );
    TestEqual(TEXT("12: one AuthFailed"), T.AuthFailed(), 1);
    TestEqual(TEXT("12: logout step"), Count(T.Log, EKind::SendLogoutStep), 1);
    TestFalse(TEXT("12: no session"), T.S.HasPlayerSession());
  }

  // 13. A new connection in the middle of a session says nothing to the
  //     game and starts from the beginning.
  {
    FScript T;
    T.InWorld();
    T.Feed(FRedwoodSessionEvent::Initialize());
    TestEqual(
      TEXT("13: connecting"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::Connecting)
    );
    TestEqual(
      TEXT("13: no word to the game"),
      T.Lost() + T.Reestablished() + T.AuthFailed(),
      0
    );
    TestFalse(TEXT("13: no session"), T.S.HasPlayerSession());
  }

  // 14. A game caller that joined the re-entry in flight hears the drop that
  //     takes it, and the resume that follows brings the realm back with no
  //     caller left waiting.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeRefused());
    T.LoginAccepted(TokenB);
    T.Enter(9);
    TestEqual(
      TEXT("14: the joined caller waits"),
      Count(T.Log, EKind::AnswerRealmEntry),
      0
    );
    T.Down();
    const FRedwoodSessionEffect *Answer = T.Last(EKind::AnswerRealmEntry);
    TestTrue(
      TEXT("14: the caller hears the drop"),
      Answer && Answer->RequestId == 9 && Answer->Text == Lost
    );
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestTrue(TEXT("14: back in the realm"), T.S.IsRealmEntered());
    TestEqual(
      TEXT("14: no caller is answered twice"),
      Count(T.Log, EKind::AnswerRealmEntry),
      1
    );
  }

  // 15. A login attempted while the socket is down fails, and leaves the
  //     session that is coming back alone.
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.LogIn(5);
    const FRedwoodSessionEffect *Answer = T.Last(EKind::AnswerLogin);
    TestTrue(
      TEXT("15: the login fails"),
      Answer && Answer->RequestId == 5 && Answer->Text == NotConnected
    );
    TestTrue(TEXT("15: the session stays"), T.S.HasPlayerSession());
    TestEqual(
      TEXT("15: nothing is forgotten"), Count(T.Log, EKind::ForgetSession), 0
    );
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestEqual(TEXT("15: the session comes back"), T.Reestablished(), 1);
  }

  // 16. A new connection asked for during an outage keeps the loss the game
  //     heard: the first connect of the new socket is also the reestablished
  //     connection (HollowedOath#2564).
  {
    FScript T;
    T.InWorld();
    T.Down();
    T.Feed(FRedwoodSessionEvent::Initialize());
    TestEqual(
      TEXT("16: connect failed"),
      FRedwoodClientSession::PhaseName(T.S.GetPhase()),
      FRedwoodClientSession::PhaseName(EPhase::ConnectFailed)
    );
    TestFalse(TEXT("16: no session"), T.S.HasPlayerSession());
    T.Up();
    TestEqual(TEXT("16: one reestablished"), T.Reestablished(), 1);
    TestEqual(
      TEXT("16: first connect reported"),
      Count(T.Log, EKind::ReportFirstConnect),
      1
    );
  }

  // 17. A character is selected in a realm. One selected before the entry
  //     is not the new realm's, so the entry forgets it, and the realm is
  //     not asked to select a character it never gave the player.
  {
    FScript T;
    T.Up();
    T.LogIn();
    T.LoginAccepted();
    T.Select();
    TestEqual(
      TEXT("17: selected in no realm"), T.S.GetSelectedCharacterId(), CharA
    );
    T.Enter();
    TestTrue(
      TEXT("17: the entry forgets it"), T.S.GetSelectedCharacterId().IsEmpty()
    );
    T.Entered();
    TestEqual(
      TEXT("17: the realm is not asked to select it"),
      Count(T.Log, EKind::SendSelectCharacter),
      0
    );
  }

  // 18. A character selected in the realm survives the drop and the resume:
  //     the held requests that go out after it act for that character.
  {
    FScript T;
    T.InWorld();
    T.Down();
    TestEqual(
      TEXT("18: kept through the drop"), T.S.GetSelectedCharacterId(), CharA
    );
    T.Up();
    T.Feed(FRedwoodSessionEvent::ResumeAccepted(RealmA, FString(), CharA));
    TestEqual(
      TEXT("18: kept through the resume"), T.S.GetSelectedCharacterId(), CharA
    );
    TestEqual(
      TEXT("18: not selected again when the session names it"),
      Count(T.Log, EKind::SendSelectCharacter),
      0
    );
  }
  return true;
}
