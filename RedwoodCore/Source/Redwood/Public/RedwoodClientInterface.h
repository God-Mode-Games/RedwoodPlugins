// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodBifrostConnection.h" // FORK(hollowed-oath)
#include "RedwoodClientSession.h" // FORK(hollowed-oath)
#include "RedwoodHeldRequests.h" // FORK(hollowed-oath): HollowedOath#2854.
#include "RedwoodPendingReplies.h" // FORK(hollowed-oath): HollowedOath#2886.
#include "RedwoodModule.h"
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
  // goes away. The shape is the ability system's attribute change delegates.
  FRedwoodPushDelegate &OnPush(const FString &Event);

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
  void ListCharacterFriends(FRedwoodListCharacterFriendsOutputDelegate OnOutput
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
  TMap<FString, FRedwoodPushDelegate> PushSubscribers;
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
  void NoteJoinLost();
  TFunction<void()> MakeLostProxyJoin(uint32 Sequence, TFunction<void()> OnLost);
  void HandleTicketingJoinReply(const FString &Error, uint32 Sequence);
  // A join whose reply was lost: the game went back to character select, so
  // its assignment must not move the player. A new join clears it.
  bool bAbandonedQueueJoin = false;
  // A join is out, or the player is in the world. Otherwise an assignment is
  // a replay for a player at character select.
  bool bAssignmentExpected = false;
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
