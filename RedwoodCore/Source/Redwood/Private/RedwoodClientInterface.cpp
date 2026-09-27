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
#include "SocketIOClient.h"
#include "TimerManager.h"

namespace {

/**
 * Translate the configured director URI scheme (`ws://` / `wss://`)
 * into the equivalent HTTP scheme (`http://` / `https://`) and return
 * the resulting origin prefix unchanged otherwise. Used by the OAuth
 * paths to bind the backend-supplied `redirectUri` to the same origin
 * the client is currently connected to.
 *
 * Replacing only the scheme prefix — rather than `FString::Replace`
 * across the whole string — keeps hostnames or paths that happen to
 * contain the substring "ws" intact.
 */
static FString GetDirectorOriginAsHttp() {
  FString Uri = URedwoodSettings::GetDirectorUri();
  if (Uri.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase)) {
    return TEXT("https://") + Uri.Mid(6);
  }
  if (Uri.StartsWith(TEXT("ws://"), ESearchCase::IgnoreCase)) {
    return TEXT("http://") + Uri.Mid(5);
  }
  // Already an http(s) URI, or something unrecognized — return as-is
  // and let the StartsWith comparison decide.
  return Uri;
}

} // namespace

void URedwoodClientInterface::Deinitialize() {
  // FORK(hollowed-oath): HollowedOath#2886. The callers go away too, so a
  // request that waits for a reply is dropped, not failed.
  DirectorReplies.Reset();
  RealmReplies.Reset();

  if (Director.IsValid()) {
    Director->ClearAllCallbacks();
    Director->Disconnect();
    ISocketIOClientModule::Get().ReleaseNativePointer(Director);
    Director = nullptr;
  }
  DirectorCloseBackoff.Reset(TimerManager); // FORK(hollowed-oath)

  ReleaseRealmSocket();

  // FORK(hollowed-oath): HollowedOath#2854. The callers are going away too, so
  // a held request is dropped, not failed.
  DirectorHeldRequests.Reset(TimerManager);
  RealmHeldRequests.Reset(TimerManager);
}

// FORK(hollowed-oath) BEGIN: shared by Deinitialize and a new handshake.
void URedwoodClientInterface::ReleaseRealmSocket() {
  if (Realm.IsValid()) {
    Realm->ClearAllCallbacks();
    Realm->Disconnect();
    ISocketIOClientModule::Get().ReleaseNativePointer(Realm);
    Realm = nullptr;
    // HollowedOath#2886. No reply comes over a released socket. After the
    // release, so a failure handler cannot reach the old socket.
    RealmReplies.FailAll();
  }
  RealmCloseBackoff.Reset(TimerManager);
}
// FORK(hollowed-oath) END

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
  FRedwoodHeldRequests &Held,
  bool bSessionEstablished,
  bool bCanSend,
  const TCHAR *NotConnectedError,
  TFunction<void()> Request,
  const TDelegate<void(const TOutput &)> &OnOutput
) {
  if (Held.HoldIfReconnecting(
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
    DirectorHeldRequests,
    Director.IsValid() && bSentDirectorConnected && HasPlayerSession(),
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
  return Gate(
    RealmHeldRequests,
    Realm.IsValid() && bSentRealmConnected && HasPlayerSession(),
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
  FRedwoodPendingReplies &Replies,
  FRedwoodReplyCallback OnReply,
  const TDelegate<void(const TOutput &)> &OnOutput
) {
  return Replies.Track(MoveTemp(OnReply), MakeLostReply(OnOutput));
}

// The travel of an assignment finished, even after the game gave up on it
// (its enter-world timer can fire during the travel). The player is in the
// world: later zone transfers must move them, and the ticket is used up.
void URedwoodClientInterface::NoteArrivedInWorld() {
  bAbandonedQueueJoin = false;
  bAssignmentExpected = true;
  bLeaveTicketingOwed = false;
}

// A join replaces any ticket the server kept, so a leave owed for it would
// cancel the new one.
void URedwoodClientInterface::NoteJoinSent() {
  bAssignmentExpected = true;
  bLeaveTicketingOwed = false;
}

void URedwoodClientInterface::FailTicketingJoin() {
  bAbandonedQueueJoin = true;
  bAssignmentExpected = false;
  bLeaveTicketingOwed = true;
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

void URedwoodClientInterface::InitializeDirectorConnection(
  FRedwoodSocketConnectedDelegate OnDirectorConnected
) {
  Director = ISocketIOClientModule::Get().NewValidNativePointer();
  bSentDirectorConnected = false;

  Director->OnEvent(
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
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  FString Uri = *URedwoodSettings::GetDirectorUri();

  Director->OnReconnectionCallback = [Uri, this](
                                       unsigned ReconnectionAttempt,
                                       unsigned AttemptDelay
                                     ) {
    DirectorCloseBackoff.NoteDropped(TimerManager); // FORK(hollowed-oath)
    if (!bSentDirectorConnected && !bSentInitialDirectorConnectionFailureLog) {
      bSentInitialDirectorConnectionFailureLog = true;
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "Unable to establish initial connection to Director at %s; will continue to try to establish connection. See SocketIO plugin logs for retry attempts."
        ),
        *Uri
      );
    } else if (!bDirectorDisconnected) {
      bDirectorDisconnected = true;
      // FORK(hollowed-oath): HollowedOath#2854. Before bAuthenticated is
      // cleared: it reads whether the player was logged in at the drop.
      NoteDirectorDrop();
      bAuthenticated = false;
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "Lost connection to Director at %s; will continue to try to reestablish connection. See SocketIO plugin logs for retry attempts."
        ),
        *Uri
      );
      OnDirectorConnectionLost.Broadcast();
    }
  };

  // FORK(hollowed-oath): only Deinitialize closes the Director, and it clears
  // the callbacks first, so every close that reaches this handler is a drop.
  Director->OnDisconnectedCallback =
    MakeUnrequestedCloseHandler(Director, DirectorCloseBackoff);

  Director->OnConnectedCallback = [Uri, OnDirectorConnected, this](
                                    const FString &InSocketId,
                                    const FString &InSessionId
                                  ) {
    NoteFirstDirectorConnect(); // FORK(hollowed-oath): HollowedOath#2854.
    DirectorCloseBackoff.NoteConnected(TimerManager); // FORK(hollowed-oath)
    NoteDirectorReconnected(); // FORK(hollowed-oath): HollowedOath#2886.
    bDirectorDisconnected = false;

    if (!bSentDirectorConnected) {
      bSentDirectorConnected = true;
      FRedwoodSocketConnected Details;
      Details.Error = TEXT("");
      OnDirectorConnected.ExecuteIfBound(Details);
      UE_LOG(LogRedwood, Log, TEXT("Connected to Director at %s"), *Uri);
    } else {
      UE_LOG(
        LogRedwood,
        Log,
        TEXT(
          "Reestablished connection to Director at %s, attempting to reauthenticate."
        ),
        *Uri
      );

      // FORK(hollowed-oath): HollowedOath#2854. A player who logged out has
      // nothing to re-login with, and a failed re-login would report an
      // authentication failure on the title screen.
      bLoggedOutDuringRelogin = false;
      if (!HasPlayerSession()) {
        OnDirectorConnectionReestablished.Broadcast();
        return;
      }

      Login(
        PlayerId,
        AuthToken,
        "local",
        true,
        FRedwoodAuthUpdateDelegate::CreateLambda([this, OnDirectorConnected](
                                                   const FRedwoodAuthUpdate
                                                     &Update
                                                 ) {
          if (Update.Type == ERedwoodAuthUpdateType::Success) {
            UE_LOG(
              LogRedwood,
              Log,
              TEXT(
                "Reauthenticated connection with Director, calling connection reestablished."
              )
            );
            // FORK(hollowed-oath): HollowedOath#2854. Before the game reacts
            // to the reconnect.
            EndDirectorReauthentication(true);
            OnDirectorConnectionReestablished.Broadcast();
          } else {
            UE_LOG(
              LogRedwood,
              Error,
              TEXT("Could not reauthenticate connection with Director: %s"),
              *Update.Message
            );
            // FORK(hollowed-oath): fire the fork-added OnDirectorAuthFailed delegate on reauth
            // failure. Upstream only logs the failure; the fork surfaces it up to the client's
            // disconnect/reconnect modal (via RedwoodClientGameSubsystem).
            // FORK(hollowed-oath): HollowedOath#2854.
            EndDirectorReauthentication(false);
            // A player who logged out meanwhile is at the title screen.
            if (!bLoggedOutDuringRelogin) {
              OnDirectorAuthFailed.Broadcast(Update.Message);
            }
          }
        }),
        true
      );
    }
  };

  UE_LOG(LogRedwood, Log, TEXT("Connecting to Director at %s"), *Uri);

  Director->Connect(*Uri);
}

bool URedwoodClientInterface::IsDirectorConnected() {
  return Director.IsValid() && Director->bIsConnected;
}

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

void URedwoodClientInterface::Register(
  const FString &Username,
  const FString &Password,
  FRedwoodAuthUpdateDelegate OnUpdate
) {
  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("Not connected to Director.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("username"), Username);
  Payload->SetStringField(TEXT("password"), Password);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("player:register:username"),
    Payload,
    TrackReply(DirectorReplies, [this, OnUpdate](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      PlayerId = MessageStruct->GetStringField(TEXT("playerId"));
      FString Error = MessageStruct->GetStringField(TEXT("error"));

      FRedwoodAuthUpdate Update;

      if (Error.IsEmpty()) {
        Update.Type = ERedwoodAuthUpdateType::Success;
        Update.Message = TEXT("");
      } else if (Error == "Must verify account") {
        OnAccountVerified = OnUpdate;
        Update.Type = ERedwoodAuthUpdateType::MustVerifyAccount;
        Update.Message = TEXT("");
      } else {
        Update.Type = ERedwoodAuthUpdateType::Error;
        Update.Message = Error;
      }

      OnUpdate.ExecuteIfBound(Update);
    }, OnUpdate)
  );
}

void URedwoodClientInterface::Logout() {
  // FORK(hollowed-oath): HollowedOath#2854. Not IsLoggedIn: in the silent grace
  // after a Director drop the player is not authenticated, and a no-op here
  // let the re-login bring the player back after they chose to leave.
  if (HasPlayerSession()) {
    // Before the ids are cleared, so the held requests fail and are not sent.
    DirectorHeldRequests.Expire(TimerManager);
    RealmHeldRequests.Expire(TimerManager);
    TimerManager.ClearTimer(ReauthenticationAttemptTimer);
    bRealmReauthPending = false;
    bOnlineCharacterOwedAfterRealm = false;
    bLoggedOutDuringRelogin = true;

    TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
    Payload->SetStringField(TEXT("playerId"), PlayerId);

    if (Director.IsValid() && Director->bIsConnected) {
      Director->Emit(TEXT("player:logout"), Payload);
    }

    // FORK(hollowed-oath): HollowedOath#2854. Disconnect a Realm socket that is
    // still reconnecting too: once back, it would start a re-handshake for a
    // player who left.
    if (Realm.IsValid()) {
      if (Realm->bIsConnected) {
        // FORK(hollowed-oath): HollowedOath#2886. Only a Realm that knows the
        // player can run the leave. In the re-login window the flag is
        // dropped below all the same; the server ticket then expires.
        if (IsRealmReady()) {
          SendOwedLeave();
        }
        Realm->Emit(TEXT("realm:auth:player:logout"), Payload);
      }
      RequestRealmClose(); // FORK(hollowed-oath)
    }

    PlayerId = TEXT("");
    AuthToken = TEXT("");
    // FORK(hollowed-oath): HollowedOath#2886. The server can replay an
    // assignment kept for the last character; nothing here may accept it.
    SelectedCharacterId = TEXT("");
    bAssignmentExpected = false;
    bAbandonedQueueJoin = false;
    // The next login can be another account, whose tickets are not ours.
    bLeaveTicketingOwed = false;
    // FORK(hollowed-oath): HollowedOath#2854. See HasPlayerSession: both
    // flags, or a later Director drop at the title screen re-logs in with
    // empty ids and reports an authentication failure there.
    bAuthenticated = false;
    bLoggedInAtDrop = false;

    URedwoodSaveGame *SaveGame = Cast<URedwoodSaveGame>(
      UGameplayStatics::CreateSaveGameObject(URedwoodSaveGame::StaticClass())
    );

    UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
  }
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
  if (!Director.IsValid() || !Director->bIsConnected) {
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
  FRedwoodAuthUpdateDelegate OnUpdate,
  bool bBypassProviderCheck
) {
  if (!Director.IsValid() || !Director->bIsConnected) {
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

  if (bBypassProviderCheck) {
    Payload->SetBoolField(TEXT("bypassProviderCheck"), true);
  }

  // FORK(hollowed-oath): HollowedOath#2886. Not tracked (HA plan ruling
  // C1): the re-login also runs this, and a failure would end the session.
  Director->Emit(
    TEXT("player:login:username"),
    Payload,
    [this, Username, bRememberMe, OnUpdate](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      PlayerId = MessageObject->GetStringField(TEXT("playerId"));
      AuthToken = MessageObject->GetStringField(TEXT("token"));
      Nickname = MessageObject->GetStringField(TEXT("nickname"));

      FRedwoodAuthUpdate Update;

      if (Error.IsEmpty()) {
        Update.Type = ERedwoodAuthUpdateType::Success;
        Update.Message = TEXT("");

        bAuthenticated = true;

        URedwoodSaveGame *SaveGame =
          Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
            URedwoodSaveGame::StaticClass()
          ));

        if (bRememberMe) {
          SaveGame->Username = Username;
          SaveGame->AuthToken = AuthToken;
        }

        UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
      } else if (Error == "Must verify account") {
        OnAccountVerified = OnUpdate;
        Update.Type = ERedwoodAuthUpdateType::MustVerifyAccount;
        Update.Message = TEXT("");
      } else {
        Update.Type = ERedwoodAuthUpdateType::Error;
        Update.Message = Error;

        URedwoodSaveGame *SaveGame =
          Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
            URedwoodSaveGame::StaticClass()
          ));
        UGameplayStatics::SaveGameToSlot(SaveGame, TEXT("RedwoodSaveGame"), 0);
      }

      OnUpdate.ExecuteIfBound(Update);
    }
  );
}

void URedwoodClientInterface::LoginWithDiscord(
  bool bRememberMe, FRedwoodAuthUpdateDelegate OnUpdate
) {
  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("Not connected to Director.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("player:login:discord:initialize"),
    Payload,
    TrackReply(DirectorReplies, [this, bRememberMe, OnUpdate](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        FString ClientId = MessageObject->GetStringField(TEXT("clientId"));
        FString State = MessageObject->GetStringField(TEXT("state"));
        FString RedirectUri =
          MessageObject->GetStringField(TEXT("redirectUri"));

        // Validate the backend-supplied redirectUri targets the
        // same origin as the director we're connected to. A
        // compromised/malicious director could otherwise send us off
        // to an attacker-controlled callback after the user grants
        // OAuth consent, leaking the auth code.
        const FString ExpectedRedirectPrefix = GetDirectorOriginAsHttp();
        if (!RedirectUri.StartsWith(
              ExpectedRedirectPrefix, ESearchCase::IgnoreCase
            )) {
          UE_LOG(
            LogRedwood,
            Error,
            TEXT(
              "Refusing Discord OAuth: redirectUri %s does not match the connected director origin %s"
            ),
            *RedirectUri,
            *ExpectedRedirectPrefix
          );
          FRedwoodAuthUpdate Update;
          Update.Type = ERedwoodAuthUpdateType::Error;
          Update.Message =
            TEXT("Login canceled: server returned an unexpected redirect URL.");
          OnUpdate.ExecuteIfBound(Update);
          return;
        }

        FString AuthorizationUrl = FString::Printf(
          TEXT(
            "https://discord.com/oauth2/authorize?response_type=code&client_id=%s&scope=identify&state=%s&redirect_uri=%s&prompt=none&integration_type=1"
          ),
          *ClientId,
          *State,
          *RedirectUri
        );

        // open the browser
        FPlatformProcess::LaunchURL(*AuthorizationUrl, nullptr, nullptr);

        TSharedPtr<FJsonObject> FinalizePayload =
          MakeShareable(new FJsonObject);
        FinalizePayload->SetStringField(TEXT("state"), State);

        // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
        Director->Emit(
          TEXT("player:login:discord:finalize"),
          FinalizePayload,
          DirectorReplies.Track(
            [this, bRememberMe, OnUpdate](auto FinalResponse) {
              TSharedPtr<FJsonObject> FinalMessageObject =
                FinalResponse[0]->AsObject();
              FString FinalError =
                FinalMessageObject->GetStringField(TEXT("error"));

              FRedwoodAuthUpdate Update;

              if (FinalError.IsEmpty()) {
                Update.Type = ERedwoodAuthUpdateType::Success;
                Update.Message = TEXT("");

                bAuthenticated = true;
                PlayerId = FinalMessageObject->GetStringField(TEXT("playerId"));
                AuthToken = FinalMessageObject->GetStringField(TEXT("token"));
                Nickname = FinalMessageObject->GetStringField(TEXT("nickname"));

                URedwoodSaveGame *SaveGame =
                  Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
                    URedwoodSaveGame::StaticClass()
                  ));

                if (bRememberMe) {
                  SaveGame->Username =
                    FinalMessageObject->GetStringField(TEXT("username"));
                  SaveGame->AuthToken = AuthToken;
                }

                UGameplayStatics::SaveGameToSlot(
                  SaveGame, TEXT("RedwoodSaveGame"), 0
                );
              } else {
                Update.Type = ERedwoodAuthUpdateType::Error;
                Update.Message = FinalError;
              }

              OnUpdate.ExecuteIfBound(Update);
            },
            MakeLostReply(OnUpdate)
          )
        );
      } else {
        FRedwoodAuthUpdate Update;
        Update.Type = ERedwoodAuthUpdateType::Error;
        Update.Message = Error;
        OnUpdate.ExecuteIfBound(Update);
      }
    }, OnUpdate)
  );
}

void URedwoodClientInterface::LoginWithTwitch(
  bool bRememberMe, FRedwoodAuthUpdateDelegate OnUpdate
) {
  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodAuthUpdate Update;
    Update.Type = ERedwoodAuthUpdateType::Error;
    Update.Message = TEXT("Not connected to Director.");
    OnUpdate.ExecuteIfBound(Update);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("player:login:twitch:initialize"),
    Payload,
    TrackReply(DirectorReplies, [this, bRememberMe, OnUpdate](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        FString ClientId = MessageObject->GetStringField(TEXT("clientId"));
        FString State = MessageObject->GetStringField(TEXT("state"));
        FString RedirectUri =
          MessageObject->GetStringField(TEXT("redirectUri"));

        // Validate the backend-supplied redirectUri targets the
        // same origin as the director we're connected to. A
        // compromised/malicious director could otherwise send us off
        // to an attacker-controlled callback after the user grants
        // OAuth consent, leaking the auth code.
        const FString ExpectedRedirectPrefix = GetDirectorOriginAsHttp();
        if (!RedirectUri.StartsWith(
              ExpectedRedirectPrefix, ESearchCase::IgnoreCase
            )) {
          UE_LOG(
            LogRedwood,
            Error,
            TEXT(
              "Refusing Twitch OAuth: redirectUri %s does not match the connected director origin %s"
            ),
            *RedirectUri,
            *ExpectedRedirectPrefix
          );
          FRedwoodAuthUpdate Update;
          Update.Type = ERedwoodAuthUpdateType::Error;
          Update.Message =
            TEXT("Login canceled: server returned an unexpected redirect URL.");
          OnUpdate.ExecuteIfBound(Update);
          return;
        }

        FString AuthorizationUrl = FString::Printf(
          TEXT(
            "https://id.twitch.tv/oauth2/authorize?response_type=code&client_id=%s&state=%s&redirect_uri=%s&scope="
          ),
          *ClientId,
          *State,
          *RedirectUri
        );

        // open the browser
        FPlatformProcess::LaunchURL(*AuthorizationUrl, nullptr, nullptr);

        TSharedPtr<FJsonObject> FinalizePayload =
          MakeShareable(new FJsonObject);
        FinalizePayload->SetStringField(TEXT("state"), State);

        // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
        Director->Emit(
          TEXT("player:login:twitch:finalize"),
          FinalizePayload,
          DirectorReplies.Track(
            [this, bRememberMe, OnUpdate](auto FinalResponse) {
              TSharedPtr<FJsonObject> FinalMessageObject =
                FinalResponse[0]->AsObject();
              FString FinalError =
                FinalMessageObject->GetStringField(TEXT("error"));

              FRedwoodAuthUpdate Update;

              if (FinalError.IsEmpty()) {
                Update.Type = ERedwoodAuthUpdateType::Success;
                Update.Message = TEXT("");

                bAuthenticated = true;
                PlayerId = FinalMessageObject->GetStringField(TEXT("playerId"));
                AuthToken = FinalMessageObject->GetStringField(TEXT("token"));
                Nickname = FinalMessageObject->GetStringField(TEXT("nickname"));

                URedwoodSaveGame *SaveGame =
                  Cast<URedwoodSaveGame>(UGameplayStatics::CreateSaveGameObject(
                    URedwoodSaveGame::StaticClass()
                  ));

                if (bRememberMe) {
                  SaveGame->Username =
                    FinalMessageObject->GetStringField(TEXT("username"));
                  SaveGame->AuthToken = AuthToken;
                }

                UGameplayStatics::SaveGameToSlot(
                  SaveGame, TEXT("RedwoodSaveGame"), 0
                );
              } else {
                Update.Type = ERedwoodAuthUpdateType::Error;
                Update.Message = FinalError;
              }

              OnUpdate.ExecuteIfBound(Update);
            },
            MakeLostReply(OnUpdate)
          )
        );
      } else {
        FRedwoodAuthUpdate Update;
        Update.Type = ERedwoodAuthUpdateType::Error;
        Update.Message = Error;
        OnUpdate.ExecuteIfBound(Update);
      }
    }, OnUpdate)
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:players:search:name"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodPlayerOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:players:search:id"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:friends:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:friends:add"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:friends:remove"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);
  Payload->SetBoolField(TEXT("accept"), bAccept);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:friends:respond-request"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("friendId"), OtherPlayerId);
  Payload->SetBoolField(TEXT("block"), bBlocked);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:friends:block"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodListRealmContactsOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:contacts:list"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
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
  Realm->Emit(
    TEXT("realm:contacts:add"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);
  Payload->SetStringField(TEXT("targetCharacterId"), OtherCharacterId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:contacts:remove"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListGuilds(
  bool bOnlyPlayersGuilds, FRedwoodListGuildsOutputDelegate OnOutput
) {
  // FORK(hollowed-oath): HollowedOath#2854. See GateDirector.
  if (GateDirector([=, this]() {
        ListGuilds(bOnlyPlayersGuilds, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodListGuildsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetBoolField(TEXT("onlyPlayersGuilds"), bOnlyPlayersGuilds);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:search"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodGetGuildOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:get"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodGetGuildOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:selected:get"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:selected:set"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:membership:join"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:membership:invite"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:membership:leave"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:membership:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:admin:create"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:admin:update"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:admin:kick"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:admin:kick"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:admin:promote"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildId"), GuildId);
  Payload->SetStringField(TEXT("targetId"), TargetId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:admin:demote"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodListAlliancesOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("guildIdFilter"), GuildIdFilter);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:alliances:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:search"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Director."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:alliances:admin:has-admin-privileges"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:admin:create"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:admin:update"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:admin:kick"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:admin:kick"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
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
  Director->Emit(
    TEXT("director:guilds:alliances:membership:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:alliances:membership:join"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("guildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:alliances:membership:leave"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FString Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("allianceId"), AllianceId);
  Payload->SetStringField(TEXT("targetGuildId"), GuildId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:guilds:alliances:membership:invite"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FString Error = MessageObject->GetStringField(TEXT("error"));

      OnOutput.ExecuteIfBound(Error);
    }, OnOutput)
  );
}

void URedwoodClientInterface::ListRealms(
  FRedwoodListRealmsOutputDelegate OnOutput
) {
  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodListRealmsOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("realm:list"),
    Payload,
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

void URedwoodClientInterface::InitializeRealmConnection(
  FRedwoodRealm InRealm, FRedwoodSocketConnectedDelegate OnRealmConnected
) {
  InitiateRealmHandshake(InRealm, OnRealmConnected);
}

void URedwoodClientInterface::InitiateRealmHandshake(
  FRedwoodRealm InRealm, FRedwoodSocketConnectedDelegate OnRealmConnected
) {
  if (!Director.IsValid() || !Director->bIsConnected || !IsLoggedIn()) {
    FRedwoodSocketConnected Output;
    Output.Error = TEXT("Not connected or authenticated to Director.");
    OnRealmConnected.ExecuteIfBound(Output);
    return;
  }

  CurrentRealm = FRedwoodRealm();
  const uint32 Generation = ++RealmHandshakeGeneration; // FORK(hollowed-oath)
  // FORK(hollowed-oath): HollowedOath#2886. The server can replay an
  // assignment kept for the last character; nothing here may accept it.
  SelectedCharacterId = TEXT("");
  bAssignmentExpected = false;
  bAbandonedQueueJoin = false;

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("realmId"), InRealm.Id);

  // FORK(hollowed-oath): HollowedOath#2886. Not tracked (HA plan ruling
  // C1): the re-login also runs this, and a failure would end the session.
  Director->Emit(
    TEXT("realm:auth:player:connect:client-to-director"),
    Payload,
    [this, InRealm, OnRealmConnected, Generation](auto Response) {
      // FORK(hollowed-oath): a late answer must not replace a newer socket.
      if (Generation != RealmHandshakeGeneration) {
        FRedwoodSocketConnected Output;
        Output.Error = TEXT("A newer Realm connection replaced this one.");
        OnRealmConnected.ExecuteIfBound(Output);
        return;
      }

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      FString Token = MessageObject->GetStringField(TEXT("token"));

      if (!Error.IsEmpty()) {
        FRedwoodSocketConnected Output;
        Output.Error = Error;
        OnRealmConnected.ExecuteIfBound(Output);
        return;
      }

      // FORK(hollowed-oath): a retry replaces the socket. Release the old one
      // first, or its callbacks still act on the new socket.
      ReleaseRealmSocket();
      CurrentRealmId = InRealm.Id;
      CurrentRealm = InRealm;
      Realm = ISocketIOClientModule::Get().NewValidNativePointer();
      bSentRealmConnected = false;
      BindRealmCloseHandler(); // FORK(hollowed-oath)
      // FORK(hollowed-oath): HollowedOath#2854. Requests held for the old
      // Realm socket cannot go to this one, so they fail now.
      bRealmReauthPending = false;
      bOnlineCharacterOwedAfterRealm = false;
      RealmHeldRequests.Expire(TimerManager);

      Realm->OnReconnectionCallback = [InRealm, this](
                                        unsigned ReconnectionAttempt,
                                        unsigned AttemptDelay
                                      ) {
        RealmCloseBackoff.NoteDropped(TimerManager); // FORK(hollowed-oath)
        if (!bSentRealmConnected && !bSentInitialRealmConnectionFailureLog) {
          bSentInitialRealmConnectionFailureLog = true;
          UE_LOG(
            LogRedwood,
            Error,
            TEXT(
              "Unable to establish initial connection to Realm at %s; will continue to try to establish connection. See SocketIO plugin logs for retry attempts."
            ),
            *InRealm.Uri
          );
        } else if (!bRealmDisconnected) {
          bRealmDisconnected = true;
          UE_LOG(
            LogRedwood,
            Error,
            TEXT(
              "Lost connection to Realm at %s; will continue to try to reestablish connection. See SocketIO plugin logs for retry attempts."
            ),
            *InRealm.Uri
          );
          NoteRealmDrop(); // FORK(hollowed-oath): HollowedOath#2854.
          OnRealmConnectionLost.Broadcast();
        }
      };

      Realm->OnConnectedCallback = [this, Token, OnRealmConnected, InRealm](
                                     const FString &InSocketId,
                                     const FString &InSessionId
                                   ) {
        NoteFirstRealmConnect(); // FORK(hollowed-oath): HollowedOath#2854.
        RealmCloseBackoff.NoteConnected(TimerManager); // FORK(hollowed-oath)
        NoteRealmReconnected(); // FORK(hollowed-oath): HollowedOath#2886.
        bRealmDisconnected = false;

        if (!bSentRealmConnected) {
          UE_LOG(
            LogRedwood, Log, TEXT("Connected to Realm at %s"), *InRealm.Uri
          );
          bSentRealmConnected = true;
          FinalizeRealmHandshake(Token, OnRealmConnected);
        } else {
          UE_LOG(
            LogRedwood,
            Log,
            TEXT(
              "Reestablished connection to Realm at %s, attempting to reauthenticate"
            ),
            *InRealm.Uri
          );
          BeginRealmReauthentication();
        }
      };

      UE_LOG(LogRedwood, Log, TEXT("Connecting to Realm at %s"), *InRealm.Uri);
      Realm->Connect(*InRealm.Uri);
    }
  );
}

void URedwoodClientInterface::BeginRealmReauthentication() {
  // FORK(hollowed-oath): HollowedOath#2854. Only a Realm drop of a live
  // session asks for a re-handshake; a Logout or a new handshake cancels it,
  // and a socket that comes back after that must not start the retry loop.
  if (!bRealmReauthPending) {
    return;
  }

  if (!Director.IsValid() || !Director->bIsConnected || !IsLoggedIn()) {
    TimerManager.SetTimer(
      ReauthenticationAttemptTimer,
      this,
      &URedwoodClientInterface::BeginRealmReauthentication,
      0.5f,
      false
    );
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("realmId"), CurrentRealmId);

  const uint32 Generation = RealmHandshakeGeneration; // FORK(hollowed-oath)
  // FORK(hollowed-oath): HollowedOath#2886. Not tracked (HA plan ruling
  // C1): the re-login also runs this, and a failure would end the session.
  Director->Emit(
    TEXT("realm:auth:player:connect:client-to-director"),
    Payload,
    [this, Generation](auto Response) {
      // FORK(hollowed-oath): the token is for a socket a new handshake replaced.
      if (Generation != RealmHandshakeGeneration) {
        return;
      }

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));
      FString Token = MessageObject->GetStringField(TEXT("token"));

      if (!Error.IsEmpty()) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Could not reauthenticate connection with Realm: %s"),
          *Error
        );
        // FORK(hollowed-oath): fire the fork-added OnRealmAuthFailed delegate on reauth failure
        // (token-fetch leg). Upstream only logs. See OnDirectorAuthFailed above.
        EndRealmReauthentication(false);
        OnRealmAuthFailed.Broadcast(Error);
        return;
      }

      FinalizeRealmHandshake(
        Token,
        FRedwoodSocketConnectedDelegate::CreateLambda(
          [this](const FRedwoodSocketConnected &Output) {
            if (Output.Error.IsEmpty()) {
              UE_LOG(
                LogRedwood,
                Log,
                TEXT(
                  "Reauthenticated connection with Realm, calling connection reestablished."
                )
              );
              EndRealmReauthentication(true);
              OnRealmConnectionReestablished.Broadcast();
            } else {
              UE_LOG(
                LogRedwood,
                Error,
                TEXT("Could not reauthenticate connection with Realm: %s"),
                *Output.Error
              );
              // FORK(hollowed-oath): fire the fork-added OnRealmAuthFailed delegate on reauth
              // failure (handshake-finalize leg). Upstream only logs. See OnDirectorAuthFailed above.
              EndRealmReauthentication(false);
              OnRealmAuthFailed.Broadcast(Output.Error);
            }
          }
        )
      );
    }
  );
}

void URedwoodClientInterface::FinalizeRealmHandshake(
  FString Token, FRedwoodSocketConnectedDelegate OnRealmConnected
) {
  BindRealmEvents();

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("token"), Token);

  const uint32 Generation = RealmHandshakeGeneration; // FORK(hollowed-oath)
  // FORK(hollowed-oath): HollowedOath#2886. Not tracked (HA plan ruling
  // C1): the re-login also runs this, and a failure would end the session.
  Realm->Emit(
    TEXT("realm:auth:player:connect:client-to-realm"),
    Payload,
    [this, OnRealmConnected, Generation](auto Response) {
      // FORK(hollowed-oath): an answer queued before its socket was replaced.
      if (Generation != RealmHandshakeGeneration) {
        return;
      }

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      // FORK(hollowed-oath): HollowedOath#2886. Before the caller can join.
      if (Error.IsEmpty()) {
        SendOwedLeave();
      }

      FRedwoodSocketConnected Output;
      Output.Error = Error;
      OnRealmConnected.ExecuteIfBound(Output);
    }
  );
}

void URedwoodClientInterface::BindRealmEvents() {
  Realm->OnEvent(
    TEXT("realm:regions"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      HandleRegionsChanged(Event, Message);
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:ticketing:update"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::Update;
      Update.Message = Message->AsObject()->GetStringField(TEXT("message"));
      OnTicketingUpdate.ExecuteIfBound(Update);
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:ticketing:ticket-error"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::TicketError;
      Update.Message = Message->AsObject()->GetStringField(TEXT("error"));
      OnTicketingUpdate.ExecuteIfBound(Update);

      OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:servers:connect-to-instance"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      // FORK(hollowed-oath): HollowedOath#2886. A zone assignment sent again
      // after a reconnect can be for a request the game dropped, for a
      // character the player no longer plays, or reach a player who is at
      // character select with no join. None may move the player.
      // An old server sends no characterId; its assignment moves as before.
      FString AssignedCharacterId;
      if (!bAssignmentExpected || bAbandonedQueueJoin ||
          (MessageObject->TryGetStringField(
             TEXT("characterId"), AssignedCharacterId
           ) &&
           !AssignedCharacterId.IsEmpty() &&
           AssignedCharacterId != SelectedCharacterId)) {
        UE_LOG(
          LogRedwood,
          Warning,
          TEXT(
            "Ignoring a zone assignment for a dropped queue request or another character."
          )
        );
        return;
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
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:parties:invites:alert"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      TSharedPtr<FJsonObject> InviteObject =
        MessageObject->GetObjectField(TEXT("invite"));
      FRedwoodPartyInvite Invite =
        URedwoodCommonGameSubsystem::ParsePartyInvite(InviteObject);

      OnPartyInvited.Broadcast(Invite);
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:parties:changed"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      TSharedPtr<FJsonObject> PartyObject =
        MessageObject->GetObjectField(TEXT("party"));
      CurrentParty = URedwoodCommonGameSubsystem::ParseParty(PartyObject);
      OnPartyUpdated.Broadcast(CurrentParty);
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:parties:kick"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      CurrentParty = FRedwoodParty();
      OnPartyKicked.Broadcast();
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
  );

  Realm->OnEvent(
    TEXT("realm:parties:emote"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      TSharedPtr<FJsonObject> MessageObject = Message->AsObject();

      FString TargetPlayerId = MessageObject->GetStringField(TEXT("playerId"));
      FString Emote = MessageObject->GetStringField(TEXT("emote"));

      OnPartyEmoteReceived.Broadcast(TargetPlayerId, Emote);
    },
    TEXT("/"),
    ESIOThreadOverrideOption::USE_GAME_THREAD
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
  return Realm.IsValid() && Realm->bIsConnected;
}

// FORK(hollowed-oath): HollowedOath#2886. For the requests the gates do not
// hold. In the re-login window the socket is back but does not know the
// player, and nothing answers a request sent to it.
bool URedwoodClientInterface::IsRealmReady() {
  return IsRealmConnected() && !bRealmReauthPending;
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodListCharactersOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:characters:list"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodListCharactersOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetBoolField(TEXT("includeArchived"), true);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:characters:list"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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
  Realm->Emit(
    TEXT("realm:characters:create"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FString Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Error);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);
  Payload->SetBoolField(TEXT("archived"), bArchived);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:characters:archive"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodGetCharacterOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), PlayerId);
  Payload->SetStringField(TEXT("characterIdOrName"), CharacterIdOrName);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:characters:get"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
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
  Realm->Emit(
    TEXT("realm:characters:set:client"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput, CharacterId](auto Response) {
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
  // select until it joins.
  if (CharacterId != SelectedCharacterId) {
    bAssignmentExpected = false;
  }
  SelectedCharacterId = CharacterId;

  if (!CurrentParty.bValid || !Realm.IsValid() || !Realm->bIsConnected) {
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("characterId"), SelectedCharacterId);

  Realm->Emit(TEXT("realm:parties:select-character"), Payload);

  // FORK(hollowed-oath): HollowedOath#2854. Same payload as the re-login path.
  const TSharedPtr<FJsonObject> OnlinePayload =
    MakeOnlineCharacterPayload(PlayerId, SelectedCharacterId, CurrentRealmId);
  if (OnlinePayload.IsValid()) {
    Director->Emit(SetOnlineCharacterEventName, OnlinePayload);
  }
}

// FORK(hollowed-oath) BEGIN: see the header.
TFunction<void(const ESIOConnectionCloseReason)>
URedwoodClientInterface::MakeUnrequestedCloseHandler(
  TWeakPtr<FSocketIONative> WeakSocket,
  FRedwoodCloseBackoff &Backoff,
  TFunction<bool()> IsCloseRequested
) {
  // Backoff is a member, and Deinitialize clears the callbacks and the
  // timers before it goes, so the reference cannot outlive it.
  return [this, WeakSocket, &Backoff, IsCloseRequested](
           const ESIOConnectionCloseReason Reason
         ) {
    TSharedPtr<FSocketIONative> Socket = WeakSocket.Pin();
    // A drop the library saw by itself is already reconnecting; it reports
    // CLOSE_REASON_DROP only after its last attempt.
    if (!Socket.IsValid() ||
        Reason != ESIOConnectionCloseReason::CLOSE_REASON_NORMAL ||
        (IsCloseRequested && IsCloseRequested())) {
      return;
    }

    const float DelaySeconds =
      Backoff.Schedule(TimerManager, [this, WeakSocket]() {
        if (TSharedPtr<FSocketIONative> Pinned = WeakSocket.Pin()) {
          ReconnectSocket(*Pinned);
        }
      });
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "The server closed a socket that the client did not ask to close; reconnecting in %.1f s (attempt %d)."
      ),
      DelaySeconds,
      Backoff.NumAttempts()
    );

    // Every close reports the drop, so the game shows the lost connection
    // until the socket is back.
    if (Socket->OnReconnectionCallback) {
      Socket->OnReconnectionCallback(
        static_cast<uint32>(Backoff.NumAttempts()),
        static_cast<uint32>(DelaySeconds * 1000.0f)
      );
    }
  };
}

// The reset matters as much as the flag: a reconnect that an earlier
// unrequested close scheduled would otherwise fire after this close, and
// bring back a Realm session that is over.
void URedwoodClientInterface::RequestRealmClose() {
  bRealmCloseRequested = true;
  RealmCloseBackoff.Reset(TimerManager);
  Realm->Disconnect();
  // HollowedOath#2886. No reply comes over a closed socket. After the close,
  // so a failure handler cannot reach the socket before it closes.
  RealmReplies.FailAll();
}

void URedwoodClientInterface::BindRealmCloseHandler() {
  bRealmCloseRequested = false;
  Realm->OnDisconnectedCallback = MakeUnrequestedCloseHandler(
    Realm, RealmCloseBackoff, [this]() { return bRealmCloseRequested; }
  );
}
// FORK(hollowed-oath) END

TSharedPtr<FJsonObject> URedwoodClientInterface::MakeOnlineCharacterPayload(
  const FString &InPlayerId,
  const FString &InCharacterId,
  const FString &InRealmId
) {
  if (InPlayerId.IsEmpty() || InCharacterId.IsEmpty() || InRealmId.IsEmpty()) {
    return nullptr;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), InPlayerId);
  Payload->SetStringField(TEXT("characterId"), InCharacterId);
  Payload->SetStringField(TEXT("realmId"), InRealmId);
  return Payload;
}

// FORK(hollowed-oath): HollowedOath#2854. A director re-login writes an online
// state with no realm, so after a director-frontend move friends saw the
// player with no character until the next character selection. Send the
// character again when the player is still in the realm. When the realm
// is not back yet, EndRealmReauthentication sends it instead.
void URedwoodClientInterface::ResendOnlineCharacter() {
  if (!IsRealmConnected() || bRealmReauthPending) {
    return;
  }

  const TSharedPtr<FJsonObject> Payload =
    MakeOnlineCharacterPayload(PlayerId, SelectedCharacterId, CurrentRealmId);
  if (!Payload.IsValid()) {
    return;
  }

  Director->Emit(SetOnlineCharacterEventName, Payload);
}

// A first connect that failed twice already broadcast a lost connection, and
// the game shows a message for it. Its success is the only event that can take
// that message down; the reconnect path broadcasts after its re-login instead.
void URedwoodClientInterface::NoteFirstDirectorConnect() {
  if (bLostBeforeFirstDirectorConnect && !bSentDirectorConnected) {
    bLostBeforeFirstDirectorConnect = false;
    OnDirectorConnectionReestablished.Broadcast();
  }
}

void URedwoodClientInterface::NoteFirstRealmConnect() {
  if (bLostBeforeFirstRealmConnect && !bSentRealmConnected) {
    bLostBeforeFirstRealmConnect = false;
    OnRealmConnectionReestablished.Broadcast();
  }
}

// The Realm socket reports connected again before the player is authenticated
// on it, so the held requests wait for the re-handshake, not for the socket. A
// first connection that keeps failing also lands here; it has no handshake to
// redo and no request to hold, but the game was told the connection is lost.
void URedwoodClientInterface::NoteRealmDrop() {
  if (bSentRealmConnected) {
    bRealmReauthPending = true;
    RealmHeldRequests.StartGrace(TimerManager);
  } else {
    bLostBeforeFirstRealmConnect = true;
  }
  // FORK(hollowed-oath): HollowedOath#2886. Last, so a request that a failure
  // handler sends again is held for the re-login.
  RealmReplies.FailAll();
}

// FORK(hollowed-oath) BEGIN: HollowedOath#2886. A request that the gates do
// not hold (JoinQueue) and that was sent after the drop was not pending when
// the drop failed the others. The library sent it on the new socket before
// the re-login, where no route answers it, so it fails here.
void URedwoodClientInterface::NoteRealmReconnected() {
  if (bRealmDisconnected) {
    RealmReplies.FailAll();
  }
}

// The server can still hold a ticket whose join reply was lost, and replay
// its assignment. Leaving deletes both. Sent at the first Realm auth after
// the loss, whatever came between; the reply to the auth is the first moment
// the Realm knows the player. The flag stays until a leave succeeds or a join
// replaces the ticket; with a join out, the leave would cancel it.
void URedwoodClientInterface::SendOwedLeave() {
  if (!bLeaveTicketingOwed || bAssignmentExpected || !IsRealmConnected()) {
    return;
  }
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Realm->Emit(
    TEXT("realm:ticketing:leave"),
    Payload,
    RealmReplies.Track([this](auto Response) {
      if (Response[0]->AsObject()->GetStringField(TEXT("error")).IsEmpty()) {
        bLeaveTicketingOwed = false;
      }
    }, []() {})
  );
}

FRedwoodReplyCallback URedwoodClientInterface::TrackDirectorReply(
  FRedwoodReplyCallback OnReply, TFunction<void()> OnLost
) {
  return DirectorReplies.Track(MoveTemp(OnReply), MoveTemp(OnLost));
}

void URedwoodClientInterface::NoteDirectorReconnected() {
  if (bDirectorDisconnected) {
    DirectorReplies.FailAll();
  }
}
// FORK(hollowed-oath) END

void URedwoodClientInterface::NoteDirectorDrop() {
  bLostBeforeFirstDirectorConnect = !bSentDirectorConnected;
  bLoggedInAtDrop = bLoggedInAtDrop || bAuthenticated;
  // Count the request grace from the drop, like the game's own disconnect
  // grace. A first connect that fails has nothing to hold.
  if (bSentDirectorConnected) {
    DirectorHeldRequests.StartGrace(TimerManager);
  }

  // A Realm re-handshake that already asked the Director for its token loses
  // the answer with this socket, and nothing would ask again: every Realm
  // request would then fail for good with no notice. Retry it the way it
  // waits for the Director, once the re-login is done.
  if (bRealmReauthPending && IsRealmConnected() &&
      !TimerManager.IsTimerActive(ReauthenticationAttemptTimer)) {
    TimerManager.SetTimer(
      ReauthenticationAttemptTimer,
      this,
      &URedwoodClientInterface::BeginRealmReauthentication,
      0.5f,
      false
    );
  }

  // FORK(hollowed-oath): HollowedOath#2886. Last, so a request that a failure
  // handler sends again is held for the re-login.
  DirectorReplies.FailAll();
}

// Not AuthToken: a failed re-login writes the EMPTY ids of its reply before it
// checks the error, and the requests of a player who was logged in must still
// fail then, not go out to a socket that does not know the player.
bool URedwoodClientInterface::HasPlayerSession() const {
  return bAuthenticated || bLoggedInAtDrop;
}

bool URedwoodClientInterface::CanSendToDirector() {
  return FRedwoodHeldRequests::CanSend(
    IsDirectorConnected(), bAuthenticated, HasPlayerSession()
  );
}

bool URedwoodClientInterface::CanSendToRealm() {
  return FRedwoodHeldRequests::CanSend(
    IsRealmConnected(), !bRealmReauthPending, HasPlayerSession()
  );
}

// Restore the online character, then send what the player asked for while the
// Director was away. A failed re-login ended the session, so the held requests
// fail now instead of at the end of the grace.
void URedwoodClientInterface::EndDirectorReauthentication(bool bSucceeded) {
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
    bLoggedInAtDrop = false;
    // A Realm not back yet cannot take it; its re-handshake sends it then.
    bOnlineCharacterOwedAfterRealm = bRealmReauthPending;
    ResendOnlineCharacter();
    DirectorHeldRequests.Release(TimerManager);
  } else {
    DirectorHeldRequests.Expire(TimerManager);
    // The Realm re-handshake needs a logged-in Director, and this session
    // cannot log in again, so its Realm session is over too. Its held requests
    // fail now, while the pending flag still refuses them. Closing the socket
    // keeps later Realm requests refused once the flag is clear, and stops a
    // reconnect that would start a retry polling for good, or re-handshake
    // the old realm after the next login.
    RealmHeldRequests.Expire(TimerManager);
    bRealmReauthPending = false;
    TimerManager.ClearTimer(ReauthenticationAttemptTimer);
    if (Realm.IsValid()) {
      RequestRealmClose(); // FORK(hollowed-oath)
    }
  }
}

// A failed re-handshake leaves bRealmReauthPending set, so the held requests
// and later ones fail instead of reaching a Realm that does not know the
// player. The next handshake clears it.
void URedwoodClientInterface::EndRealmReauthentication(bool bSucceeded) {
  // A retry armed by a Director drop must not start a second handshake.
  TimerManager.ClearTimer(ReauthenticationAttemptTimer);

  if (bSucceeded) {
    bRealmReauthPending = false;
    // Only when a Director re-login found the Realm not back yet: after a
    // Realm-only drop, the online state still has the character.
    if (bOnlineCharacterOwedAfterRealm && IsDirectorConnected() && bAuthenticated) {
      ResendOnlineCharacter();
    }
    bOnlineCharacterOwedAfterRealm = false;
    RealmHeldRequests.Release(TimerManager);
  } else {
    RealmHeldRequests.Expire(TimerManager);
  }
}

void URedwoodClientInterface::JoinMatchmaking(
  FString ProfileId,
  TArray<FString> InRegions,
  FRedwoodTicketingUpdateDelegate OnUpdate
) {
  bAbandonedQueueJoin = false; // FORK(hollowed-oath): HollowedOath#2886.

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
  // FORK(hollowed-oath): HollowedOath#2886. A new request replaces the one
  // whose reply was lost.
  bAbandonedQueueJoin = false;

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
  Realm->Emit(
    TEXT("realm:ticketing:join:queue"),
    Payload,
    RealmReplies.Track([this](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
      Update.Message = Error;
      OnTicketingUpdate.ExecuteIfBound(Update);

      if (!Error.IsEmpty()) {
        OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
      }
    }, [this]() { FailTicketingJoin(); })
  );
}

void URedwoodClientInterface::JoinCustom(
  bool bTransferWholeParty,
  TArray<FString> InRegions,
  FRedwoodTicketingUpdateDelegate OnUpdate
) {
  bAbandonedQueueJoin = false; // FORK(hollowed-oath): HollowedOath#2886.

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
  Realm->Emit(
    TEXT("realm:ticketing:join:custom"),
    Payload,
    RealmReplies.Track([this](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
      Update.Message = Error;
      OnTicketingUpdate.ExecuteIfBound(Update);

      if (!Error.IsEmpty()) {
        OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
      }
    }, [this]() { FailTicketingJoin(); })
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
  Realm->Emit(
    TEXT("realm:ticketing:leave"),
    Payload,
    RealmReplies.Track([this, OnOutput](auto Response) {
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
  Realm->Emit(
    TEXT("realm:servers:list-proxies"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:servers:create-proxy"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
      FRedwoodCreateProxyOutput Output;

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      Output.Error = MessageObject->GetStringField(TEXT("error"));

      MessageObject->TryGetStringField(
        TEXT("proxyReference"), Output.ProxyReference
      );

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
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
  Realm->Emit(
    TEXT("realm:servers:join-proxy"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
      FRedwoodJoinServerOutput Output;

      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      Output.Error = MessageObject->GetStringField(TEXT("error"));

      MessageObject->TryGetStringField(
        TEXT("connectionUri"), Output.ConnectionUri
      );
      MessageObject->TryGetStringField(TEXT("token"), Output.Token);

      OnOutput.ExecuteIfBound(Output);
    }, OnOutput)
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
  Realm->Emit(
    TEXT("realm:servers:stop-proxy"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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
  Realm->Emit(
    TEXT("realm:ticketing:join:matchmaking"),
    Payload,
    RealmReplies.Track([this](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();
      FString Error = MessageObject->GetStringField(TEXT("error"));

      FRedwoodTicketingUpdate Update;
      Update.Type = ERedwoodTicketingUpdateType::JoinResponse;
      Update.Message = Error;
      OnTicketingUpdate.ExecuteIfBound(Update);

      if (!Error.IsEmpty()) {
        OnTicketingUpdate = FRedwoodTicketingUpdateDelegate();
      }
    }, [this]() { FailTicketingJoin(); })
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
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
  Realm->Emit(
    TEXT("realm:parties:get:frontend"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:parties:leave"),
    Payload,
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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
  // FORK(hollowed-oath): HollowedOath#2854. See GateRealm.
  if (GateRealm([=, this]() {
        InviteToParty(TargetPlayerId, OnOutput);
      }, OnOutput)) {
    return;
  }

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:parties:invites:initiate"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodListPartyInvitesOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:parties:invites:get"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
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
  Realm->Emit(
    TEXT("realm:parties:invites:respond"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:parties:promote"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    OnOutput.ExecuteIfBound(TEXT("Not connected to Realm."));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:parties:kick"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
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
  Realm->Emit(
    TEXT("realm:parties:set"),
    Payload,
    TrackReply(RealmReplies, [OnOutput](auto Response) {
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
  if (!Realm.IsValid() || !Realm->bIsConnected || !CurrentParty.bValid) {
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

  Realm->Emit(TEXT("realm:parties:emote"), Payload);
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

  if (!Director.IsValid() || !Director->bIsConnected) {
    FRedwoodGetGlobalDataOutput Output;
    Output.Error = TEXT("Not connected to Director.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Director->Emit(
    TEXT("director:global-data:get:latest"),
    TrackReply(DirectorReplies, [this, OnOutput](auto Response) {
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

  if (!Realm.IsValid() || !Realm->bIsConnected) {
    FRedwoodGetGlobalDataOutput Output;
    Output.Error = TEXT("Not connected to Realm.");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);

  // FORK(hollowed-oath): HollowedOath#2886. See TrackReply.
  Realm->Emit(
    TEXT("realm:global-data:get:latest"),
    TrackReply(RealmReplies, [this, OnOutput](auto Response) {
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
