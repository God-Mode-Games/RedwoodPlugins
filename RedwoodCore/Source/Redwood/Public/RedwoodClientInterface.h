// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodBifrostConnection.h" // FORK(hollowed-oath)
#include "RedwoodClientSession.h" // FORK(hollowed-oath)
#include "RedwoodHeldRequests.h" // FORK(hollowed-oath): HollowedOath#2854.
#include "RedwoodModule.h"
#include "RedwoodPendingReplies.h" // FORK(hollowed-oath): HollowedOath#2886.
#include "Types/RedwoodTypes.h"

#include "CoreMinimal.h"
#include "SIOJsonObject.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "TimerManager.h"

#include "RedwoodClientInterface.generated.h"

// FORK(hollowed-oath): a Redwood push, as URedwoodClientInterface::OnPush
// hands it to a subscriber: the push's payload object.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FRedwoodPushDelegate, const TSharedPtr<FJsonObject> & /*Payload*/
);

// FORK(hollowed-oath) BEGIN: chat over the session (protocol.md, "Chat").
// A chat line the connection parsed, and a list of chat topics.
DECLARE_MULTICAST_DELEGATE_OneParam(
  FRedwoodChatMessageDelegate, const FRedwoodChatMessage & /*Message*/
);
DECLARE_MULTICAST_DELEGATE_OneParam(
  FRedwoodChatTopicsDelegate, const TArray<FString> & /*Topics*/
);

// The answer to a chat send. Error is the text the player reads, empty on
// success; Code is the contract's error code beside it (muted, not_available,
// too_long, forbidden, rate_limited, invalid_request, unavailable), so the
// game can act on one. MsgId is the line's id, on every answer, so a send
// that failed can be sent again as the same line. Topic and Seq name the
// posted line. A reply lost with the socket has the client's own code,
// LostReplyCode: the line may or may not have posted, and the game resends
// it once under the same id. A send the gate refused ("Not connected to
// Realm.") has an empty Code: nothing is resent into a closed gate.
struct REDWOOD_API FRedwoodChatSendOutput {
  static constexpr const TCHAR *LostReplyCode = TEXT("lost_reply");

  FString Error;
  FString Code;
  FString MsgId;
  FString Topic;
  int64 Seq = 0;
};
typedef TDelegate<void(const FRedwoodChatSendOutput &)>
  FRedwoodChatSendDelegate;
// FORK(hollowed-oath) END

// This class is separated from URedwoodClientGameSubsystem
// so that we can use it in automated tests without instantiating
// a whole world.

UCLASS()
class REDWOOD_API URedwoodClientInterface : public UObject,
                                            public FTickableGameObject {
  GENERATED_BODY()

public:
  void Deinitialize();

  // FTickableGameObject begin
  virtual void Tick(float DeltaTime) override;
  virtual TStatId GetStatId() const override;
  virtual bool IsTickableInEditor() const override {
    return true;
  }
  // FTickableGameObject end

  // FORK(hollowed-oath): the client holds ONE connection, to Bifrost, which
  // carries the Director and the Realm. The names below keep the steps a
  // caller takes: connect, log in, enter a realm. "Director connected" is
  // the socket; "Realm connected" is the session being in a realm.
  void InitializeDirectorConnection(
    FRedwoodSocketConnectedDelegate OnDirectorConnected
  );

  bool IsDirectorConnected();

  // FORK(hollowed-oath): HollowedOath#2886. The client world of a game
  // server, or a standalone world (the entry level), began play. See the .cpp.
  void NoteArrivedInWorld();
  void NoteLeftWorld();

  FRedwoodDynamicDelegate OnPingsReceived;

  FRedwoodConnectToServerDynamicDelegate OnRequestToJoinServer;
  FRedwoodStitchToServerDynamicDelegate OnRequestToStitchServer;

  // FORK(hollowed-oath): one connection, so one set. Lost: the socket
  // dropped, and its requests are held for the reconnect. Reestablished: the
  // session is back (resumed, or logged in again with the realm and the
  // character restored). AuthFailed: the session is over and cannot come
  // back by itself; the message says why. Upstream had no auth-failure
  // surface (it only logged); the game's disconnect modal needs it.
  FRedwoodDynamicDelegate OnConnectionLost;
  FRedwoodDynamicDelegate OnConnectionReestablished;
  FRedwoodConnectionAuthFailedDynamicDelegate OnAuthFailed;

  // FORK(hollowed-oath): the subscribers of one Redwood push, by its event
  // name. The game owns features that ride on pushes this plugin does not
  // interpret (a staff broadcast, for one), and Bifrost forwards only the
  // events on its push allowlist, so a new event is agreed there and bound
  // here, with no change to the plugin. The subscription lives on this
  // object, not on the connection: it holds over a new connection and a
  // resume, with no re-bind by the caller. A push the plugin handles itself
  // reaches its own handler first, then the subscribers. Add with the
  // delegate's Add methods and Remove with the handle before the subscriber
  // goes away. The reference stays valid for the life of this object, so a
  // subscriber may subscribe to another event from inside its own callback.
  // The shape is the ability system's attribute change delegates.
  FRedwoodPushDelegate &OnPush(const FString &Event);

  // FORK(hollowed-oath) BEGIN: chat over the session. A send is gated like a
  // realm request: during a drop it is held, and goes out after the chat is
  // resumed, so the player's line lands behind the lines they missed. The
  // line's id is made before the gate, so a held send goes out with the id
  // it was given and Bifrost posts it once. A whisper names a player; Bifrost
  // writes it to both sides' character topics. The overloads with MsgId are
  // for the game's own retry of a send that failed: the same id, and Bifrost
  // posts the line once however many times it is sent.
  void SendChat(
    const FString &Topic, const FString &Body, FRedwoodChatSendDelegate OnOutput
  );
  void SendChat(
    const FString &Topic,
    const FString &Body,
    const FString &MsgId,
    FRedwoodChatSendDelegate OnOutput
  );
  void SendWhisper(
    const FString &ToPlayerId,
    const FString &Body,
    FRedwoodChatSendDelegate OnOutput
  );
  void SendWhisper(
    const FString &ToPlayerId,
    const FString &Body,
    const FString &MsgId,
    FRedwoodChatSendDelegate OnOutput
  );
  // A whisper to a character by its name, as the player typed it.
  void SendWhisperByName(
    const FString &ToName, const FString &Body, FRedwoodChatSendDelegate OnOutput
  );
  void SendWhisperByName(
    const FString &ToName,
    const FString &Body,
    const FString &MsgId,
    FRedwoodChatSendDelegate OnOutput
  );
  void ReportChat(
    const FString &Topic,
    int64 Seq,
    const FString &Reason,
    FRedwoodErrorOutputDelegate OnOutput
  );
  // Every chat line this client had not seen: live, or replayed after a
  // drop. Each line of a topic comes once, after the lines before it.
  FRedwoodChatMessageDelegate OnChatMessage;
  // The topics the session can read and send to: each time Bifrost changes
  // the set, and when the session ends (empty).
  FRedwoodChatTopicsDelegate OnChatTopics;
  // Topics whose lines may be missing after a drop: the replay was cut
  // short, or the resume was refused or lost and the cursors moved on with
  // the live lines. Nothing is retried; the UI can say lines were missed.
  FRedwoodChatTopicsDelegate OnChatTruncated;
  const TArray<FString> &GetChatTopics() const {
    return ChatTopics;
  }
  // What the player reads for a chat refusal, by the contract's code. A
  // code with no text of its own shows the server's message, and so does
  // `muted`: the server's message names the end of the mute and its reason,
  // which no fixed text can.
  static FString ChatErrorText(const FString &Code, const FString &Message);
  // FORK(hollowed-oath) END

  void Register(
    const FString &Username,
    const FString &Password,
    FRedwoodAuthUpdateDelegate OnUpdate
  );

  void AttemptAutoLogin(FRedwoodAuthUpdateDelegate OnUpdate);

  // FORK(hollowed-oath): the login is the join of the Bifrost session.
  // Provider is "hams_access" (the launcher's code) or "local" (a token
  // from an earlier login, as the auto-login and the re-login send).
  void Login(
    const FString &Username,
    const FString &PasswordOrToken,
    const FString &Provider,
    bool bRememberMe,
    FRedwoodAuthUpdateDelegate OnUpdate
  );

  FString GetNickname();

  void Logout();

  bool IsLoggedIn();

  FString GetPlayerId();
  FString GetCharacterId();
  FString GetCharacterName();
  FString GetRealmId();

  void CancelWaitingForAccountVerification();

  void SearchForPlayers(
    FString UsernameOrNickname,
    bool bIncludePartialMatches,
    FRedwoodListPlayersOutputDelegate OnOutput
  );
  void SearchForPlayerById(
    FString TargetPlayerId, FRedwoodPlayerOutputDelegate OnOutput
  );

  void ListFriends(
    ERedwoodFriendListType Filter, FRedwoodListPlayersOutputDelegate OnOutput
  );

  void RequestFriend(
    FString OtherPlayerId, FRedwoodErrorOutputDelegate OnOutput
  );

  void RemoveFriend(
    FString OtherPlayerId, FRedwoodErrorOutputDelegate OnOutput
  );

  void RespondToFriendRequest(
    FString OtherPlayerId, bool bAccept, FRedwoodErrorOutputDelegate OnOutput
  );

  void SetPlayerBlocked(
    FString OtherPlayerId, bool bBlocked, FRedwoodErrorOutputDelegate OnOutput
  );

  void ListRealmContacts(FRedwoodListRealmContactsOutputDelegate OnOutput);

  void AddRealmContact(
    FString OtherCharacterId,
    bool bBlocked,
    FRedwoodErrorOutputDelegate OnOutput
  );

  void RemoveRealmContact(
    FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
  );

  // FORK(hollowed-oath) BEGIN: character friend calls. Upstream has no
  // friendship between characters; the routes live in the RedwoodBackend fork.
  void ListCharacterFriends(
    FRedwoodListCharacterFriendsOutputDelegate OnOutput
  );

  void RequestCharacterFriend(
    FString TargetCharacterId, FRedwoodErrorOutputDelegate OnOutput
  );

  void RespondToCharacterFriendRequest(
    FString OtherCharacterId, bool bAccept, FRedwoodErrorOutputDelegate OnOutput
  );

  void RemoveCharacterFriend(
    FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
  );

  // Removes only the caller's pending request, so a cancel cannot end a
  // friendship the other side accepted meanwhile.
  void CancelCharacterFriendRequest(
    FString OtherCharacterId, FRedwoodErrorOutputDelegate OnOutput
  );
  // FORK(hollowed-oath) END

  // FORK(hollowed-oath) BEGIN: account-wide chat mutes, the GM's /mute and
  // /unmute. Director routes, because a mute is the account's, not a realm's.
  // The director takes the commander from the session, never from a field, and
  // checks their role itself: a GM may set a mute with an end, an Admin one
  // with no end (DurationMs 0). The target is a character name, so a GM can
  // mute a player in another zone; the director resolves it to a player and
  // refuses a name of no character in play. Both answer the director's error
  // word for word, because the game owns what a GM reads.
  void SetMute(
    FString TargetCharacterName,
    int64 DurationMs,
    FString Reason,
    FRedwoodSetMuteOutputDelegate OnOutput
  );

  void ClearMute(
    FString TargetCharacterName, FRedwoodErrorOutputDelegate OnOutput
  );

  // The parse half of SetMute, public so a test can feed it an answer. An
  // answer with no error string is a bad answer, not a success.
  static FRedwoodSetMuteOutput ParseSetMuteAnswer(
    const TArray<TSharedPtr<FJsonValue>> &Response
  );
  // FORK(hollowed-oath) END

  void ListGuilds(
    bool bOnlyPlayersGuilds, FRedwoodListGuildsOutputDelegate OnOutput
  );

  void SearchForGuilds(
    FString SearchText,
    bool bIncludePartialMatches,
    FRedwoodListGuildsOutputDelegate OnOutput
  );

  void GetGuild(FString GuildId, FRedwoodGetGuildOutputDelegate OnOutput);

  void SetSelectedGuild(FString GuildId, FRedwoodErrorOutputDelegate OnOutput);

  void GetSelectedGuild(FRedwoodGetGuildOutputDelegate OnOutput);

  void JoinGuild(FString GuildId, FRedwoodErrorOutputDelegate OnOutput);

  void InviteToGuild(
    FString GuildId,
    FString TargetPlayerId,
    FRedwoodErrorOutputDelegate OnOutput
  );

  void LeaveGuild(FString GuildId, FRedwoodErrorOutputDelegate OnOutput);

  void ListGuildMembers(
    FString GuildId,
    ERedwoodGuildAndAllianceMemberState State,
    FRedwoodListGuildMembersOutputDelegate OnOutput
  );

  void CreateGuild(
    FString GuildName,
    FString GuildTag,
    ERedwoodGuildInviteType InviteType,
    bool bListed,
    bool bMembershipPublic,
    FRedwoodCreateGuildOutputDelegate OnOutput
  );

  void UpdateGuild(
    FString GuildId,
    FString GuildName,
    FString GuildTag,
    ERedwoodGuildInviteType InviteType,
    bool bListed,
    bool bMembershipPublic,
    FRedwoodErrorOutputDelegate OnOutput
  );

  void KickPlayerFromGuild(
    FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
  );

  void BanPlayerFromGuild(
    FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
  );

  void UnbanPlayerFromGuild(
    FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
  );

  void PromotePlayerToGuildAdmin(
    FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
  );

  void DemotePlayerFromGuildAdmin(
    FString GuildId, FString TargetId, FRedwoodErrorOutputDelegate OnOutput
  );

  void ListAlliances(
    FString GuildIdFilter, FRedwoodListAlliancesOutputDelegate OnOutput
  );

  void SearchForAlliances(
    FString SearchText,
    bool bIncludePartialMatches,
    FRedwoodListAlliancesOutputDelegate OnOutput
  );

  void CanAdminAlliance(
    FString AllianceId, FRedwoodErrorOutputDelegate OnOutput
  );

  void CreateAlliance(
    FString AllianceName,
    FString GuildId,
    bool bInviteOnly,
    FRedwoodCreateAllianceOutputDelegate OnOutput
  );

  void UpdateAlliance(
    FString AllianceId,
    FString AllianceName,
    bool bInviteOnly,
    FRedwoodErrorOutputDelegate OnOutput
  );

  void KickGuildFromAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void BanGuildFromAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void UnbanGuildFromAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void ListAllianceGuilds(
    FString AllianceId,
    ERedwoodGuildAndAllianceMemberState State,
    FRedwoodListAllianceGuildsOutputDelegate OnOutput
  );

  void JoinAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void LeaveAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void InviteGuildToAlliance(
    FString AllianceId, FString GuildId, FRedwoodErrorOutputDelegate OnOutput
  );

  void ListRealms(FRedwoodListRealmsOutputDelegate OnOutput);

  // FORK(hollowed-oath): one realm:list entry, public so a test can feed it
  // JSON without a director.
  static FRedwoodRealm ParseRealm(const TSharedPtr<FJsonObject> &RealmObj);

  void InitializeConnectionForFirstRealm(
    FRedwoodSocketConnectedDelegate OnRealmConnected
  );
  void InitializeRealmConnection(
    FRedwoodRealm InRealm, FRedwoodSocketConnectedDelegate OnRealmConnected
  );

  bool IsRealmConnected(FRedwoodRealm &OutRealm);
  bool IsRealmConnected();

  TMap<FString, float> GetRegions();

  void ListCharacters(FRedwoodListCharactersOutputDelegate OnOutput);

  void ListArchivedCharacters(FRedwoodListCharactersOutputDelegate OnOutput);

  void CreateCharacter(
    FString Name,
    USIOJsonObject *CharacterCreatorData,
    FRedwoodGetCharacterOutputDelegate OnOutput
  );

  void SetCharacterArchived(
    FString CharacterId, bool bArchived, FRedwoodErrorOutputDelegate OnOutput
  );

  void GetCharacterData(
    FString CharacterIdOrName, FRedwoodGetCharacterOutputDelegate OnOutput
  );

  void SetCharacterData(
    FString CharacterId,
    FString Name,
    USIOJsonObject *CharacterCreatorData,
    FRedwoodGetCharacterOutputDelegate OnOutput
  );

  // FORK(hollowed-oath): one step of the session: the realm checks that the
  // player owns the character and selects it for the party, then the
  // director records the online state. It replaces the two sends of
  // upstream (realm:parties:select-character and the director's online
  // state), which could disagree.
  void SetSelectedCharacter(FString CharacterId);

  void JoinMatchmaking(
    FString ProfileId,
    TArray<FString> InRegions,
    FRedwoodTicketingUpdateDelegate OnUpdate
  );

  void JoinQueue(
    FString ProxyId,
    FString ZoneName,
    bool bTransferWholeParty,
    bool bFavorLastZone,
    FRedwoodTicketingUpdateDelegate OnUpdate
  );

  void JoinCustom(
    bool bTransferWholeParty,
    TArray<FString> InRegions,
    FRedwoodTicketingUpdateDelegate OnUpdate
  );

  void LeaveTicketing(FRedwoodErrorOutputDelegate OnOutput);

  void ListProxies(
    TArray<FString> PrivateProxyReferences,
    FRedwoodListProxiesOutputDelegate OnOutput
  );
  void CreateProxy(
    bool bJoinSession,
    FRedwoodCreateProxyInput Parameters,
    FRedwoodCreateProxyOutputDelegate OnOutput
  );
  void JoinProxyWithSingleInstance(
    FString ProxyReference,
    FString Password,
    FRedwoodJoinServerOutputDelegate OnOutput
  );
  void StopProxy(FString ServerProxyId, FRedwoodErrorOutputDelegate OnOutput);

  FString GetConnectionConsoleCommand();
  FURL GetConnectionURL();

  // FORK(hollowed-oath): the "director:friends:character-alert" push.
  FRedwoodCharacterFriendAlertDynamicDelegate OnCharacterFriendAlert;

  FRedwoodPartyInvitedDynamicDelegate OnPartyInvited;
  FRedwoodPartyUpdatedDynamicDelegate OnPartyUpdated;
  FRedwoodDynamicDelegate OnPartyKicked;
  FRedwoodPartyEmoteReceivedDynamicDelegate OnPartyEmoteReceived;
  FRedwoodParty GetCachedParty() {
    return CurrentParty;
  }
  // FORK(hollowed-oath): HollowedOath#2448. Applies a realm:parties:changed
  // roster to the held party. False when the roster is stale and must not be
  // broadcast. See the .cpp.
  static bool ApplyPartyChange(
    FRedwoodParty &InOutHeld, const FRedwoodParty &Changed
  );
  void GetOrCreateParty(
    bool bCreateIfNotInParty, FRedwoodGetPartyOutputDelegate OnOutput
  );
  void LeaveParty(FRedwoodErrorOutputDelegate OnOutput);
  void InviteToParty(
    FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
  );
  void ListPartyInvites(FRedwoodListPartyInvitesOutputDelegate OnOutput);
  void RespondToPartyInvite(
    FString PartyId, bool bAccept, FRedwoodGetPartyOutputDelegate OnOutput
  );
  void PromoteToPartyLeader(
    FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
  );
  void KickFromParty(
    FString TargetPlayerId, FRedwoodErrorOutputDelegate OnOutput
  );
  void SetPartyData(
    FString LootType,
    USIOJsonObject *PartyData,
    FRedwoodGetPartyOutputDelegate OnOutput
  );
  void SendEmoteToParty(FString Emote);

  void GetDirectorGlobalData(FRedwoodGetGlobalDataOutputDelegate OnOutput);
  void GetRealmGlobalData(FRedwoodGetGlobalDataOutputDelegate OnOutput);

private:
  // Before the connection and the request holders: members are destroyed in
  // reverse order, and theirs clear timers on this manager.
  FTimerManager TimerManager;

  // FORK(hollowed-oath) BEGIN: the one connection and the session on it.
  // FRedwoodClientSession decides everything about the session; this object
  // feeds it what happened (Dispatch) and does what it says (Execute).
  TSharedPtr<FRedwoodBifrostConnection> Connection;
  FString ConnectionUri;
  FRedwoodClientSession Session;
  // An event raised while effects run (a game delegate calls back in) waits
  // its turn, so one row's effects never interleave with another's.
  TArray<FRedwoodSessionEvent> EventQueue;
  bool bDispatching = false;
  void Dispatch(const FRedwoodSessionEvent &Event);
  void Execute(const FRedwoodSessionEffect &Effect);
  // The game's calls that wait for an answer, by the id the machine echoes.
  uint32 NextRequestId = 0;
  TMap<uint32, FRedwoodAuthUpdateDelegate> LoginCallers;
  TMap<uint32, FRedwoodSocketConnectedDelegate> EntryCallers;
  TMap<uint32, FRedwoodRealm> EntryRealms;
  FRedwoodSocketConnectedDelegate FirstConnectCaller;
  // The held requests' grace, counted from the drop.
  FTimerHandle GraceTimer;
  void BindPushHandlers();
  void BindRealmPushHandlers();
  // Each delegate is heap-allocated so it never moves: a broadcast runs in
  // place, and a subscriber that subscribes to a new event from its callback
  // grows the map under that broadcast.
  TMap<FString, TUniquePtr<FRedwoodPushDelegate>> PushSubscribers;
  // The chat: the topics Bifrost named last, the one path of a send (a
  // topic, a player or a character name, never two), and the resume the
  // machine asks for.
  TArray<FString> ChatTopics;
  void SendChatLine(
    const FString &MsgId,
    const FString &Topic,
    const FString &ToPlayerId,
    const FString &ToName,
    const FString &Body,
    FRedwoodChatSendDelegate OnOutput
  );
  void ResumeChat();
  // The resume failed: every topic with a cursor may be missing lines.
  void ReportChatGap();
  // The effects that reach Bifrost.
  void SendLogin(const FRedwoodSessionEffect &Effect);
  void SendEnterRealm(uint32 Seq, const FString &RealmId);
  void ApplyRegions(const TSharedPtr<FJsonObject> &Response);
  void SaveCredentials(const FString &Username, const FString &Token);
  // What of the realm is not the machine's: the ticketing cluster's flags,
  // the party and the game's realm details.
  void ClearRealmDetails();
  // FORK(hollowed-oath) END

  // FORK(hollowed-oath): the one path of the character friend calls: gated
  // like every Realm request, and needs a selected character. CharacterId is
  // read at call time, so a call held over a reconnect is not sent, and an
  // answer is not given, for a character selected after it.
  template <typename TOutput>
  void EmitCharacterFriendCall(
    const FString &EventName,
    const FString &CharacterId,
    TSharedPtr<FJsonObject> Payload,
    TOutput (*ParseAnswer)(const TArray<TSharedPtr<FJsonValue>> &),
    const TDelegate<void(const TOutput &)> &OnOutput
  );

  // FORK(hollowed-oath): HollowedOath#2448. See the .cpp.
  void SendSelectCharacter();
  template <typename TOutput>
  bool Gate(
    ERedwoodRequestKind Kind,
    TFunction<void()> Request,
    const TDelegate<void(const TOutput &)> &OnOutput
  );
  template <typename TOutput>
  bool GateDirector(
    TFunction<void()> Request, const TDelegate<void(const TOutput &)> &OnOutput
  );
  template <typename TOutput>
  bool GateRealm(
    TFunction<void()> Request, const TDelegate<void(const TOutput &)> &OnOutput
  );
  // FORK(hollowed-oath): HollowedOath#2886. See the .cpp.
  template <typename TOutput>
  FRedwoodReplyCallback TrackReply(
    FRedwoodReplyCallback OnReply,
    const TDelegate<void(const TOutput &)> &OnOutput
  );
  void FailTicketingJoin(uint32 Sequence);
  // FORK(hollowed-oath): HollowedOath#2929. See the .cpp.
  bool GateTicketingAttempt(TFunction<void()> Attempt);
  friend class FRedwoodClientTestAccess; // FORK(hollowed-oath): the tests.
  FRedwoodHeldRequests HeldRequests;
  // FORK(hollowed-oath) BEGIN: HollowedOath#2886. The requests still waiting
  // for a reply; a drop fails them.
  FRedwoodPendingReplies Replies;
  bool IsRealmReady();
  // bForce: the session machine knows the realm was entered, where the phase
  // has moved on already (a logout pays the leave on its way out).
  // AsPlayerId is given when the session's own id is already gone (a
  // logout pays its leave after the machine cleared the session).
  void SendOwedLeave(const FString &AsPlayerId, bool bForce = false);
  // The invite as the character that asked for it; a held invite keeps that
  // character through the gate.
  void InviteToPartyAs(
    const FString &InviterCharacterId,
    const FString &TargetPlayerId,
    FRedwoodErrorOutputDelegate OnOutput
  );
  void NoteJoinSent();
  void NoteJoinAnswered(const FString &Error, uint32 Sequence);
  void NoteJoinRefused();
  void NoteJoinLost();
  // HollowedOath#3609. See the .cpp.
  void NoteTicketingJoinSent();
  TSharedPtr<FJsonObject> NoteTicketingJoinAccepted(const FString &TicketId);
  TSharedPtr<FJsonObject> TakeHeldAssignment(const FString &TicketId);
  TSharedPtr<FJsonObject> TakeHeldAssignment(const FString &TicketId);
  void EndOwnJoin();
  void HandleZoneAssignment(const TSharedPtr<FJsonObject> &MessageObject);
  void HoldAssignment(
    const FString &TicketId, const TSharedPtr<FJsonObject> &Assignment
  );
  TFunction<void()> MakeLostProxyJoin(
    uint32 Sequence, TFunction<void()> OnLost
  );
  void HandleTicketingJoinReply(
    const TSharedPtr<FJsonObject> &Reply, uint32 Sequence
  );
  // A join whose reply was lost: the game went back to character select, so
  // its assignment must not move the player. A proxy join, a ticketing join
  // that the realm accepts, or a join refused in the world clears it.
  bool bAbandonedQueueJoin = false;
  // The realm accepted a join or named a party ticket, or the player is in
  // the world. Otherwise an assignment is a replay for a player at character
  // select.
  bool bAssignmentExpected = false;
  // A ticketing join is out and the realm has not answered it.
  // HollowedOath#3609: the server replays a dropped ticket's assignment
  // before it answers the join that replaces the ticket, so the guard opens
  // at the answer, not the send. The owed leave still waits for this join.
  // See NoteTicketingJoinSent.
  bool bJoinAwaitingReply = false;
  // The assignments that name a ticket, pushed while the join is awaited:
  // the realm pushes before it answers. One per ticket, the newest, oldest
  // tickets first; the answer's ticket picks the join's own. See
  // HoldAssignment.
  TArray<TPair<FString, TSharedPtr<FJsonObject>>> HeldAssignments;
  // The ticket of this client's own accepted ticketing join, from the
  // realm's answer, until its travel arrives. While it is set, another
  // ticket's assignment is a replay. Empty for an older realm that names
  // no ticket.
  FString OwnTicketId;
  // The travel of an accepted assignment arrived, and the player has not
  // left the world since. A refused or ended join does not clear it: the
  // server's zone transfers must still move the player.
  bool bInWorld = false;
  // A lost join left a ticket on the server. See SendOwedLeave.
  bool bLeaveTicketingOwed = false;
  // An accepted assignment's travel has not arrived yet. See
  // NoteArrivedInWorld. LeaveTicketing keeps it: the game's timer can fire
  // during the travel, which can still arrive.
  bool bTravelPending = false;
  // Counts the joins sent. The travel keeps the count of its join, so an
  // arrival does not clear the flags of a newer join.
  uint32 JoinSequence = 0;
  uint32 TravelJoinSequence = 0;
  // FORK(hollowed-oath) END
  // FORK(hollowed-oath) BEGIN: HollowedOath#3002. A member's client sends no
  // join when the leader queues the whole party; the server's "party queued"
  // notice is that member's join. See HandlePartyQueued.
  FString AcceptedPartyTicketId;
  // Party tickets that ended: left, or their assignment was accepted. The
  // server can send "queued" for one again (HollowedOath#3008, #3031).
  TArray<FString> EndedPartyTicketIds;
  TArray<FString> RecentPartyTicketMessageIds;
  // A "queued" that came before a character was selected: the server
  // replays it at the realm join, before character select. Selecting that
  // character accepts it.
  FString PendingPartyTicketId;
  FString PendingPartyCharacterId;
  void AcceptPartyTicket(const FString &TicketId);
  // Answers true when TicketId was the accepted ticket.
  bool EndPartyTicket(const FString &TicketId);
  void HandlePartyQueued(const TSharedPtr<FJsonObject> &Message);
  void HandlePartyLeft(const TSharedPtr<FJsonObject> &Message);
  void ForgetPartyTickets();
  // FORK(hollowed-oath) END

  void HandleRegionsChanged(
    const FString &Event, const TSharedPtr<FJsonValue> &Message
  );

  void InitiatePings();
  UFUNCTION()
  void HandlePingResult(FString TargetAddress, float RTT);

  TMap<FString, TSharedPtr<FRedwoodRegionLatency>> Regions;
  TMap<FString, float> PingAverages;
  FTimerHandle PingTimer;
  int PingAttemptsLeft;

  void AttemptJoinMatchmaking();
  void AttemptJoinCustom();
  FRedwoodTicketingUpdateDelegate OnTicketingUpdate;
  FString TicketingProfileId;
  TArray<FString> TicketingRegions;
  bool bTicketingTransferWholeParty = false;

  FString ServerConnection;
  FString ServerToken;

  FRedwoodAuthUpdateDelegate OnAccountVerified;
  // The player whose login waits for its email verification: a refused login
  // makes no session, so the push that confirms it is matched here.
  FString MustVerifyPlayerId;
  // The realm the session entered, as the game described it; the session
  // machine holds its id and the player's ids.
  FRedwoodRealm CurrentRealm;

  TMap<FString, FString> CharacterNamesById;

  UPROPERTY()
  FRedwoodParty CurrentParty;
};
