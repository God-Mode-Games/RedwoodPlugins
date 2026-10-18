// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodGameplayTags.h"
#include "RedwoodModule.h"
#include "Types/RedwoodTypes.h"
// FORK(hollowed-oath): currency entries ride the item flush.
#include "RedwoodCurrencyTypes.h"

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "GameFramework/GameplayMessageSubsystem.h"
#include "SocketIOFunctionLibrary.h"
#include "SocketIONative.h"

#include "RedwoodServerGameSubsystem.generated.h"

class AGameModeBase;
class URedwoodSyncItemAsset;
class URedwoodSyncComponent;
// FORK(hollowed-oath): forward decls for the item flush / trade helpers' parameters
// (fork-added).
class URedwoodCharacterComponent;
class URedwoodPlayerStateComponent;

// FORK(hollowed-oath) BEGIN: currency ledger types for the item flush (fork-added).
// The fields of a realm:characters:items:flush reply. An error reply means the batch was NOT
// saved, whatever CommittedSeq says.
struct FRedwoodItemFlushReply {
  FString Error;
  // -1 when the reply carried no fence (a dropped socket, a timeout).
  int64 CommittedSeq = -1;
  bool bFinal = false;
  bool bRebaseRequired = false;
  // The backend's answer to a probe whose seq never committed: nothing was written.
  bool bNotApplied = false;
  // The writer token this session sent is stale: another session owns the character.
  bool bWriterFenceLost = false;
  // Set by the sidecar: it sent the batch and no answer came, so the batch MAY have applied.
  bool bTimedOut = false;
  // Set by the sidecar: it had no realm to send the batch to, so the batch did NOT apply.
  bool bNotForwarded = false;
  // Set by the realm: it could not tell whether its own commit went through. Read like bTimedOut.
  bool bCommitUnknown = false;
  TArray<TSharedPtr<FJsonObject>> Alerts;
};

// What one flush claimed and sent, kept for its reply.
struct FRedwoodItemFlushSent {
  int64 BatchSeq = 0;
  TArray<TPair<FString, uint64>> Upserts;
  TArray<TPair<FString, uint64>> Deletes;
  int32 CurrencyCount = 0;
  // The keys themselves, not only their count: a commit adds them to the snapshot's GrantKeys.
  TArray<FString> GrantKeys;
  // The last sent entry's BalanceAfter: the balance the backend holds once this batch commits.
  bool bHasBalanceAfter = false;
  int64 BalanceAfter = 0;
};

// How a realm:characters:items:load reply ends a reload attempt.
enum class ERedwoodCurrencyReloadResult : uint8 {
  Applied,
  // A failed load that a later attempt can fix (a realm hiccup, a malformed reply).
  Retry,
  // A load no attempt can fix: the realm says this session's writer token is stale
  // (`writerFenceLost`), so another session owns the character.
  GiveUp,
};

// One flush attempt held behind a reload, resumed with its settle when the reload lands.
struct FRedwoodHeldItemFlush {
  TWeakObjectPtr<URedwoodCharacterComponent> CharacterComponent;
  TWeakObjectPtr<URedwoodPlayerStateComponent> PlayerStateComponent;
  // Null for a live component's tick flush.
  TFunction<void()> OnAckSettled;
};

// The reload of one CHARACTER: every component of that character is held while it runs, so a
// pawn-owned component cannot send an item row whose debit waits on the PlayerState-owned one.
struct FRedwoodCurrencyReload {
  // The components that asked for the reload (IsCurrencyRebaseRequired) and take its result.
  TArray<TWeakObjectPtr<URedwoodCharacterComponent>> CharacterComponents;
  TWeakObjectPtr<URedwoodPlayerStateComponent> PlayerStateComponent;
  TArray<FRedwoodHeldItemFlush> HeldFlushes;
  int32 Attempts = 0;
  // The attempt whose reply is awaited; 0 when none is. A reply for any other attempt is late and
  // is ignored: it may predate a trade that the current attempt's balance includes.
  uint64 Generation = 0;
  // Set when the sidecar was down at send time. The reconnect sends it again; a wait for the
  // sidecar spends no attempt, so a short outage cannot make a detached pawn give up.
  bool bWaitingForSidecar = false;
  FTimerHandle RetryTimer;
};

// The fields of a realm:characters:items:trade reply.
struct FRedwoodItemTradeReply {
  // False when the reply could not be read at all.
  bool bReadable = false;
  FString Error;
  // -1 when the reply carried no fence, as every error reply does.
  int64 FromCommittedSeq = -1;
  int64 ToCommittedSeq = -1;
  // Set by the sidecar: it sent the trade and no answer came, so it MAY have applied.
  bool bTimedOut = false;
  // Set by the sidecar: it had no realm to send the trade to, so it did NOT apply.
  bool bNotForwarded = false;
  // Set by the realm: it could not tell whether its own commit went through, so it MAY have applied.
  bool bCommitUnknown = false;
  TArray<TSharedPtr<FJsonObject>> Alerts;
};

// What one trade reply proves.
enum class ERedwoodItemTradeOutcome : uint8 {
  // The trade is committed: by this attempt, or by an earlier one that this resend replays.
  Committed,
  // The realm refused this attempt and wrote nothing.
  Refused,
  // The sidecar did not pass this attempt to the realm.
  NotForwarded,
  // The realm may have committed it or not: a timeout, a commit the realm could not confirm, an
  // unreadable reply, or a success without a fence past each side's seq.
  Unknown,
};

// One character of a trade, as the send saw it.
struct FRedwoodItemTradeSide {
  FString CharacterId;
  TWeakObjectPtr<URedwoodCharacterComponent> CharacterComponent;
  TWeakObjectPtr<URedwoodPlayerStateComponent> PlayerStateComponent;
  // The committed seq and balance when the trade went out. Nothing else of this character can
  // commit while the trade holds it, so the trade's own commit is SeqBefore + 1.
  int64 SeqBefore = 0;
  int64 BalanceBefore = 0;
  // This side's entries, with BalanceAfter chained from BalanceBefore. They are never in the flush
  // queues: the trade is its own atomic write.
  TArray<FRedwoodCurrencyEntry> Entries;
};

// A trade that went out and has not settled. It holds every flush of both characters until it
// does, through every resend.
struct FRedwoodPendingItemTrade {
  FRedwoodItemTradeSide From;
  FRedwoodItemTradeSide To;
  // Every attempt sends exactly this: the backend knows a resend by its trade id and entry ids.
  TSharedPtr<FJsonObject> Payload;
  TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete;
  TArray<FRedwoodHeldItemFlush> HeldFlushes;
  int32 Attempts = 0;
  // The attempt whose reply is awaited; 0 when none is. Any other reply is late and is ignored.
  uint64 Generation = 0;
  // An attempt may have committed. From then on only a success settles the trade as committed,
  // and nothing settles it as not applied.
  bool bOutcomeUnknown = false;
  // Set when the sidecar was down at a resend. The reconnect sends it, and no attempt is spent.
  bool bWaitingForSidecar = false;
  FTimerHandle Timer;
};
// FORK(hollowed-oath) END

UCLASS(BlueprintType)
class REDWOOD_API URedwoodServerGameSubsystem : public UGameInstanceSubsystem {
  GENERATED_BODY()

public:
  // Begin USubsystem
  virtual void Initialize(FSubsystemCollectionBase &Collection) override;
  virtual void Deinitialize() override;
  // End USubsystem

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void CallExecCommandOnAllClients(const FString &Command);

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString RequestId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString RealmName;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ProxyId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString InstanceId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString Name;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString MapId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ModeId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  bool bContinuousPlay = false;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString Password;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ShortCode;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  int32 MaxPlayers = 0;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString OwnerPlayerId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString Channel;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ZoneName;

  // 1-based index of which instance of the zone this server is running
  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ShardName;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString ParentProxyId;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  FString SidecarUri;

  /**
   * Travel the specified player to a new zone transform.
   * @param bShouldStitch This is a WIP feature that you likely
   * don't have access to; leave it set to false.
   */
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void TravelPlayerToZoneTransform(
    APlayerController *PlayerController,
    const FString &InZoneName,
    const FTransform &InTransform,
    const FString &OptionalProxyId = TEXT(""),
    bool bShouldStitch = false
  );

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void TravelPlayerToZoneSpawnName(
    APlayerController *PlayerController,
    const FString &InZoneName,
    const FString &InSpawnName = TEXT("default"),
    const FString &OptionalProxyId = TEXT("")
  );

  // FORK(hollowed-oath): shared handling of the sidecar's answer to both
  // TravelPlayerToZone* emits. Upstream repeats the same block in each
  // function and kicks the player on every error; this decides between a
  // kick and a rollback. Full rationale on the definition in the .cpp.
  // Public only so ZoneTravelHardeningTest can drive it without a sidecar.
  // EmitGeneration is the player state component's TransferGeneration at the
  // time of the emit; the answer is only acted on while that transfer is
  // still the one in flight.
  void HandleTransferZoneResponse(
    const TSharedPtr<FJsonObject> &Response,
    TWeakObjectPtr<APlayerController> WeakPlayerController,
    int64 EmitGeneration
  );

  // FORK(hollowed-oath): wire name of the fork-added transfer-failed
  // sidecar event. Named and pinned by a test, because the backend pins its
  // own copy of this string and the two cannot share a constant: a silent
  // drift on either side would turn the whole rollback off with nothing to
  // show for it.
  static constexpr const TCHAR *TransferFailedEventName =
    TEXT("realm:servers:transfer-zone:transfer-failed");

  // FORK(hollowed-oath): the pair both TravelPlayerToZone* early returns
  // give AbortTransferring when the sidecar is down. Named here, not at
  // file scope, because the unity build merges translation units and a
  // file-scope name can collide. SidecarDownError repeats the text of the
  // upstream log line above each early return on purpose; upstream owns
  // that line, so it keeps its own literal. SidecarDownReason is a
  // contract: the game matches on the token, so keep it stable (see
  // AbortTransferring in RedwoodPlayerStateComponent.h for the token list).
  static constexpr const TCHAR *SidecarDownError =
    TEXT("Sidecar is not connected; cannot travel player to new zone");
  static constexpr const TCHAR *SidecarDownReason = TEXT("sidecar-down");

  // FORK(hollowed-oath): body of the fork-added transfer-failed sidecar
  // event, kept out of InitializeSidecar so the fork stays a few lines
  // there. Full rationale on the definition in the .cpp. Public for the
  // same reason as HandleTransferZoneResponse above.
  void HandleTransferFailedEvent(const TSharedPtr<FJsonObject> &Payload);

  // FORK(hollowed-oath): how long a transfer waits for the sidecar's answer
  // before the player is taken out. The answer itself is quick: the route
  // answers when the ticket goes in the queue, and the wait for a zone that
  // starts on demand happens AFTER the answer. This is therefore only a
  // guard for a sidecar that dies or stops answering, so it can be generous.
  static constexpr float TransferAnswerTimeoutSeconds = 30.f;

  // FORK(hollowed-oath): runs when a transfer got no answer in
  // TransferAnswerTimeoutSeconds. Upstream has nothing here: a sidecar that
  // dies after the connection test above gives no answer and no failure
  // event, and the player then keeps the transferring latch for the rest of
  // the session, with no linkdead retention and no lastLocation. An answer
  // that never came is ambiguous by this plugin's own rule — the realm may
  // hold the character already — so this kicks, exactly like an ambiguous
  // answer. Full rationale on the definition in the .cpp. Public for the
  // same reason as HandleTransferZoneResponse above.
  void HandleTransferAnswerTimeout(
    TWeakObjectPtr<APlayerController> WeakPlayerController, int64 EmitGeneration
  );

  void FlushSync();
  void FlushPersistence();
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void FlushPlayerCharacterData(
    TArray<APlayerState *> PlayerArray, bool bForce
  );
  TSharedPtr<FJsonObject> CreatePlayerCharacterDataObject(
    APlayerState *PlayerState, bool bForce
  );
  void FlushZoneData();

  // FORK(hollowed-oath) BEGIN: per-item persistence channel helper declarations. Fork-added;
  // definitions + full rationale under a matching FORK marker in RedwoodServerGameSubsystem.cpp.
  // Replaces the earlier per-container flush helpers (FlushContainersForCharacterComponent /
  // AppendOfflineContainerRows, RedwoodPlugins#17) now that persistence is per-item flat rows
  // instead of opaque per-container blobs. Backend counterpart lives on RedwoodBackend's
  // feat/item-persistence line (realm:characters:items:{flush,trade} routes; the one-shot
  // blob->rows migrate route was deleted with the native-model pivot). Preserve
  // signatures on merge.
  //
  // What: FlushItemsForCharacterComponent sends CharacterComponent's currently-dirty item records
  // (plus pending deletions) to realm:characters:items:flush under a single-flight, seq-fenced
  // protocol-v2 envelope. Why v2: the backend rejects seq gaps and no-ops replays, so at most one
  // batch may be outstanding -- TryBeginItemFlush claims the slot and a batchSeq, and the ack's
  // CompleteItemFlush clears only the (id, generation) pairs actually sent and advances the
  // committed seq. A failed / unacknowledged / component-destroyed send leaves the dirty state set
  // so the next flush retries it; the parked-tick case (slot busy) simply coalesces more dirt.
  // Preserve: the single-flight guard, the generation-matched ack, and the TWeakObjectPtr guard on
  // the async callback.
  //
  // PlayerStateComponent supplies the payload's characterId (PlayerStateComponent->RedwoodCharacter
  // .Id). It may be null on the detached-flush leg (a retained linkdead pawn has no live
  // PlayerState); the characterId then comes from CharacterComponent->RedwoodCharacterId, which the
  // dispatch site has already verified equal to the authoritative id.
  //
  // OnAckSettled (optional) fires EXACTLY ONCE per call, when this flush attempt has SETTLED --
  // meaning the in-flight attempt resolved (success OR error), NOT that it committed. It runs after
  // CompleteItemFlush in the async ack callback (both success and error), and synchronously on
  // every early-return path that sends nothing (not dirty, sidecar down, unresolved records array,
  // single-flight slot busy, empty build), so a caller can always count on receiving it. The
  // detached-flush release barrier relies on this to order the binding-releasing player-left behind
  // the item write (see FlushDetachedCharacterData): the backend's player-left tears down the
  // character->instance binding this flush's auth gate needs, so the release must not fire until the
  // flush has settled. Null (the default) for the ordinary tick-flush caller, which needs no
  // completion signal.
  // FORK(hollowed-oath): while the component needs a currency rebase the attempt is held, and
  // OnAckSettled fires from the resumed flush after the currency reload, or when the reload gives
  // up (see CurrencyReloadMaxAttempts). A reply that sets the rebase hands its settle on the same
  // way.
  //
  // Item LOADING has no counterpart here: item rows arrive in the SAME round trip as the rest of
  // the character (see FRedwoodCharacterBackend::Items), populated directly in
  // RedwoodPlayerStateCharacterUpdated() before OnRedwoodCharacterUpdated broadcasts, instead of a
  // separate later-arriving realm:characters:items:load call.
  void FlushItemsForCharacterComponent(
    URedwoodPlayerStateComponent *PlayerStateComponent,
    URedwoodCharacterComponent *CharacterComponent,
    TFunction<void()> OnAckSettled = nullptr
  );

  // Offline/PIE counterpart to FlushItemsForCharacterComponent: appends CharacterComponent's item
  // records to OutRows, which the caller writes ONCE to CharacterObject's "items" field for
  // FlushPlayerCharacterData to put in the character's JSON file. Appends rather than writing the
  // field itself because "items" is a single field fed by potentially several character components
  // -- writing per component would keep only the last one's rows. Returns whether this component
  // contributed (false = items disabled, or a misconfigured records array), so the caller can
  // leave the field absent entirely when nothing owns items.
  //
  // Unlike the backend leg this writes EVERY record rather than only the dirty ones -- the disk
  // save is a whole-file rewrite, so a dirty-only write would drop every untouched item -- and it
  // therefore does not need the generation-tracked ack the sidecar round trip does.
  bool AppendOfflineItemRows(
    TArray<TSharedPtr<FJsonValue>> &OutRows,
    URedwoodCharacterComponent *CharacterComponent
  );

  // FORK(hollowed-oath): offline/PIE counterpart of the currency half of the item flush. Takes the
  // component's staged entries and grant keys and applies them to the character snapshot, which
  // the save then writes to the file (URedwoodCommonGameSubsystem::WriteOfflineCurrency). It refuses the whole batch, as the
  // backend would, when an entry would take the balance below zero or past int64, does not end at
  // its BalanceAfter, has no reason or an id the file already applied, or a key is already used: the refused batch is dropped and
  // logged, and the balance does not move. An unknown balance applies nothing and keeps the
  // queues. Returns true when the snapshot changed.
  static bool ApplyOfflineCurrency(
    FRedwoodCharacterBackend &Character, URedwoodCharacterComponent *CharacterComponent
  );

  // Emit realm:characters:items:trade: atomically moves the given root items (each identified by a
  // (Domain, Slot) placement, optionally with a ParentId for a content-child destination -- see
  // FRedwoodTradeRootPlacement) from one character to another on the backend. RootPlacements name
  // only the root rows being handed over; the backend re-parents each root and its nested children
  // in one transaction, and bumps BOTH characters' InventorySeq.
  //
  // FORK(hollowed-oath): the trade also moves currency, and this function owns both characters'
  // flush lanes while it runs.
  //  - CurrencyByCharacter: each entry names its character (From or To). Give each entry its Id
  //    (MakeCurrencyEntryId), Delta and the reason Currency.Reason.Trade; the deltas must sum to
  //    zero. BalanceAfter is ignored: it is chained here from the committed balance on the
  //    character's snapshot, the only balance the backend can check it against. A side that pays
  //    or is paid must have no currency staged at send time (flush it first): the trade commits
  //    before anything staged, so a staged debit could go below zero once the trade has applied.
  //  - TradeId: the game's id for the trade (32 uppercase hex digits). Make a FRESH one for every
  //    trade (MakeCurrencyEntryId). It is the backend's transfer id and receipt key, and the backend
  //    knows a resend by it: a resend with other contents is refused, and a new trade under a
  //    committed id would be acked as that old trade's replay. An id this server already sent is
  //    refused before send.
  //  - Both components need a PlayerState snapshot (FindPlayerStateComponent).
  //
  // Refused before anything is sent (OnComplete runs before this returns, with an error and -1
  // fences) when either character is frozen, has lost its writer fence, is in another trade or a
  // currency reload, has a batch in flight or unconfirmed, or when the currency is not valid.
  //
  // While the trade runs, every flush attempt of either character is held, and resumed with its
  // settle when the trade settles. On success, each side's committed seq and balance are adopted
  // on its component and snapshot (the caller must NOT re-seed), entries staged meanwhile are
  // re-chained from the new balance, and OnCurrencyRebased tells the game. An error means NOT
  // applied: nothing is dropped or applied, and staged entries are re-chained from the balance
  // before the trade. A trade whose outcome is unknown (a timeout, or a commit the realm could not
  // confirm) is sent again, byte for byte, with the lanes still held, until a reply settles it
  // (TradeResendSeconds, TradeMaxAttempts): the backend acks a committed one from its receipt. A
  // trade whose outcome cannot be learned (no settling reply after every attempt, or the server
  // shutting down) freezes both characters like a final refusal: each one's
  // OnItemFlushRefusedFinal fires and OnComplete gets an error. The game must then send both
  // players through character select. Alerts the trade raised go to OnCurrencyAlerts of the
  // character they name.
  void EmitItemsTrade(
    URedwoodCharacterComponent *FromCharacterComponent,
    URedwoodCharacterComponent *ToCharacterComponent,
    const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
    const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
    const FString &TradeId,
    TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete
  );

  // FORK(hollowed-oath): the old call shape, kept only so the game module builds and loads until
  // its trade moves to the overload above (currency ledger, Phase C). It sends nothing: without the
  // writer tokens the backend refuses every trade anyway. OnComplete gets an error before this
  // returns. Remove it with that move.
  void EmitItemsTrade(
    const FString &FromCharacterId,
    const FString &ToCharacterId,
    const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
    TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete
  );

  // Static payload builders for the two item emits above (flush + trade), split out so
  // ItemPersistenceTest can pin the wire envelope without a live sidecar connection. The sidecar
  // validates each request against its full schema BEFORE stamping its own instanceId over "id"
  // (mirroring realm:characters:set:server), so every envelope MUST carry a placeholder id --
  // omitting it (or sending JSON null) fails that pre-stamp validation and the message never
  // reaches the realm; this was live-broken for both routes on the first deployed channel ("id is
  // a required field") because no test covered the envelope layer. Emit sites MUST build their
  // payloads through these.
  // FORK(hollowed-oath): the flush also carries the claimed currency entries and grant keys, and
  // the session's writer token on every batch (the backend commits nothing for another token).
  static TSharedPtr<FJsonObject> BuildItemsFlushPayload(
    const FString &CharacterId,
    int64 BatchSeq,
    const TArray<TSharedPtr<FJsonValue>> &Upserts,
    const TArray<TSharedPtr<FJsonValue>> &Deletes,
    const TArray<FRedwoodCurrencyEntry> &CurrencyEntries,
    const TArray<FString> &GrantKeys,
    const FString &WriterToken,
    bool bProbe = false
  );
  // FORK(hollowed-oath): the trade also carries both sides' writer tokens (the backend commits
  // nothing without them), the trade id, and the currency entries, each with its character.
  static TSharedPtr<FJsonObject> BuildItemsTradePayload(
    const FString &FromCharacterId,
    const FString &ToCharacterId,
    const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
    const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
    const FString &FromWriterToken,
    const FString &ToWriterToken,
    const FString &TradeId
  );

  // FORK(hollowed-oath): the trade reply's parse and what it proves. Unknown and NotForwarded come
  // from the reply's flags (`timedOut`, `commitUnknown`, `notForwarded`; see
  // ReadSidecarDeliveryFlags for an older sidecar). A success is Committed only with a fence past
  // each side's seq before the trade: no other fence can come from this trade.
  static FRedwoodItemTradeReply ParseItemsTradeReply(const TSharedPtr<FJsonObject> &Reply);
  static ERedwoodItemTradeOutcome ClassifyItemsTradeReply(
    const FRedwoodItemTradeReply &Reply, int64 FromSeqBefore, int64 ToSeqBefore
  );

  // FORK(hollowed-oath): checks the trade currency and chains each side's BalanceAfter from its
  // BalanceBefore, into the sides' Entries. Refuses (false, with OutError) what the backend would
  // refuse: an entry for a character outside the trade, a bad or repeated id, a reason other than
  // TradeCurrencyReason, a zero delta, a balance that would go below zero or past int64, or deltas
  // that do not sum to zero.
  static bool ChainTradeCurrency(
    const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
    FRedwoodItemTradeSide &From,
    FRedwoodItemTradeSide &To,
    FString &OutError
  );

  // FORK(hollowed-oath): the lowest balance this character can hold once its unsettled trade
  // settles: the balance after the trade when it pays, the balance before it when it is paid.
  // Entries staged meanwhile are re-chained from whichever comes true, so each must fit on the
  // lower one (QueueCurrency checks). False when no unsettled trade moves this character's
  // currency.
  bool GetUnsettledTradeFloorBalance(const FString &CharacterId, int64 &OutFloorBalance) const;

  // FORK(hollowed-oath): the sidecar's `timedOut` and `notForwarded` reply flags. On a sidecar
  // that predates them, the reply's error text is matched instead, with a Warning: these are the
  // texts sidecar/src/routes/game-server/items.ts sends. A reply with either one carries a filler
  // committedSeq of 0, which is not a fence.
  static void ReadSidecarDeliveryFlags(
    const TSharedPtr<FJsonObject> &Reply,
    const FString &Error,
    const TCHAR *TimedOutError,
    bool &OutTimedOut,
    bool &OutNotForwarded
  );
  static constexpr const TCHAR *FlushTimedOutError = TEXT("Item flush request timed out.");
  static constexpr const TCHAR *SidecarRealmNotConnectedError = TEXT("Realm server not connected");

  // FORK(hollowed-oath): the realm's two trade refusals that run before its transaction and depend
  // on this server's session (its sidecar socket, the characters' binding), so a resend can get one
  // although an earlier attempt committed (realm-backend/src/routes/sidecar/items.ts). Every other
  // refusal is the same on every attempt or comes after the realm checked for a commit. The realm
  // has no flag for these, so the texts are matched.
  static constexpr const TCHAR *TradeNotBoundError =
    TEXT("both characters must be bound to this instance");
  static constexpr const TCHAR *TradeNotAuthorizedError = TEXT("not authorized");
  static bool IsItemsTradeSessionRefusal(const FString &Error);

  // The only reason the backend takes on a trade entry.
  static constexpr const TCHAR *TradeCurrencyReason = TEXT("Currency.Reason.Trade");
  // The sidecar's text when its realm request for a trade timed out. Read only from a sidecar too
  // old to send the `timedOut` flag (see ReadSidecarDeliveryFlags).
  static constexpr const TCHAR *TradeTimedOutError = TEXT("Item trade request timed out.");
  // A trade with no reply by then counts as timed out. The sidecar answers within its own realm
  // timeout (10 seconds), so a longer silence means the socket dropped and no reply will come.
  static constexpr float TradeReplyTimeoutSeconds = 30.f;
  // The wait before a trade with an unknown outcome is sent again. A realm that timed out may still
  // be running the first attempt, and a resend now would only queue behind it on the same rows; the
  // wait gives it time to finish, so the resend gets its replay ack instead of another timeout.
  static constexpr float TradeResendSeconds = 5.f;
  // After this many attempts with no settling reply the trade gives up and freezes both
  // characters. A wait for the sidecar does not count. Without a limit, a held detached pawn's
  // write binding would never release.
  static constexpr int32 TradeMaxAttempts = 6;

  // FORK(hollowed-oath): currency ledger steps of the item flush, static so the tests can
  // drive them without a sidecar.
  //
  // Copies exactly the entries and keys the component says the next batch must carry, from the
  // front of its queues. Read right after TryBeginItemFlush: the counts depend on the claim (an
  // unconfirmed batch is re-sent exactly as it went out).
  static void ClaimCurrencyToSend(
    const URedwoodCharacterComponent *CharacterComponent,
    TArray<FRedwoodCurrencyEntry> &OutEntries,
    TArray<FString> &OutGrantKeys
  );

  static FRedwoodItemFlushReply ParseItemsFlushReply(const TSharedPtr<FJsonObject> &Reply);

  // FORK(hollowed-oath): the snapshot half of a COMMITTED reply: the seq (forward only), the
  // balance (or the unknown mark behind a later fence) and the grant keys. Writes nothing for an
  // error or notApplied reply. Also used when the component that sent the batch is gone.
  static void WriteCommittedFlushToSnapshot(
    FRedwoodCharacterBackend &Snapshot,
    const FRedwoodItemFlushReply &Reply,
    const FRedwoodItemFlushSent &Sent
  );

  // Hands the reply to CompleteItemFlush and keeps the PlayerState snapshot (nullable) in step:
  // the committed seq and balance on success, a lowered seq on a sequence gap. Broadcasts the
  // reply's alerts.
  static void ApplyItemsFlushReply(
    URedwoodCharacterComponent *CharacterComponent,
    URedwoodPlayerStateComponent *PlayerStateComponent,
    const FRedwoodItemFlushReply &Reply,
    const FRedwoodItemFlushSent &Sent
  );

  // Reads the balance and the used grant keys off a realm:characters:items:load reply. Anything
  // but Applied leaves both outputs unusable.
  static ERedwoodCurrencyReloadResult ParseCurrencyReloadReply(
    const TSharedPtr<FJsonObject> &Reply, int64 &OutBalance, TArray<FString> &OutGrantKeys
  );

  // Applies a parsed reload to one component that asked for it: rebases its held currency from
  // Balance, writes the balance to the snapshot and merges the grant keys into it, and tells the game
  // (OnCurrencyRebased). Returns false when the rebase cannot apply; the component and the
  // snapshot are then unchanged and it stays held.
  static bool ApplyCurrencyReload(
    URedwoodCharacterComponent *CharacterComponent,
    URedwoodPlayerStateComponent *PlayerStateComponent,
    int64 Balance,
    const TArray<FString> &GrantKeys
  );

  // FORK(hollowed-oath): the per-character freeze after a final refusal. A frozen character sends
  // nothing and takes no new currency until a fresh hydration from the database thaws it.
  bool IsCharacterFrozen(const FString &CharacterId) const {
    return FrozenCharacterIds.Contains(CharacterId);
  }
  // Called by the auth leg when the character is loaded fresh from the database (a new sign-in
  // after character select), which is the repair. Nothing else thaws: a hydration from an existing
  // snapshot (a respawn) may come from one the freeze could not mark.
  void ThawCharacter(const FString &CharacterId) {
    FrozenCharacterIds.Remove(CharacterId);
  }

  // A failed reload is retried on a timer: a reload fails for a realm hiccup or a sidecar
  // reconnect, which clear in seconds, and nothing else would re-run it for a detached pawn.
  static constexpr float CurrencyReloadRetrySeconds = 5.f;
  // A reload with no reply by then counts as failed. The sidecar answers within its own realm
  // timeout (10 seconds), so a longer silence means the socket dropped and no reply will come;
  // without this a detached pawn's barrier would wait on it forever.
  static constexpr float CurrencyReloadReplyTimeoutSeconds = 30.f;
  // After this many failed attempts the held flush attempts settle, so a detached pawn's write
  // binding still releases when the realm keeps failing the load. A wait for the sidecar does not
  // count. The give-up logs every entry it leaves unsaved.
  static constexpr int32 CurrencyReloadMaxAttempts = 6;
  // FORK(hollowed-oath) END

  // FORK(hollowed-oath): fork-added API (with EmitPlayerLeft below), not in
  // upstream Redwood. Part of linkdead pawn retention (HollowedOath#1365,
  // called from ULinkdeadPawnManager::FlushAndEndPresence). Keep across
  // upstream merges.
  //
  // Persist a detached pawn's character data by explicit identity. For pawns
  // that outlive their player's logout (e.g. a retained "linkdead" body): the
  // normal flush only reaches a pawn through PlayerState->GetPawn(), which no
  // longer exists for these. Backend mode serializes only the field groups
  // dirtied since the last flush (per-field merge — never overwrites newer
  // backend values with a stale whole object) and emits
  // realm:characters:set:server; offline mode serializes every enabled group
  // and saves the whole document to disk. The component's RedwoodCharacterId
  // must match CharacterId or nothing is sent.
  //
  // bReleaseBindingWhenSettled additionally releases the character's
  // retained write binding (see EmitPlayerLeft) once the flush has SETTLED:
  // the player-left is emitted from the flush's sidecar acknowledgment —
  // which the sidecar only sends after the realm has fully processed the
  // write — so the release can never overtake the flush and revoke the
  // binding it still needs. Every skip path that doesn't put a write in
  // flight (no component, id mismatch, nothing dirty, offline) releases
  // immediately; a disconnected sidecar drops BOTH (and keeps the dirty
  // flags set) since neither message could be delivered anyway.
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void FlushDetachedCharacterData(
    APawn *Pawn,
    const FString &CharacterId,
    const FString &PlayerId,
    bool bReleaseBindingWhenSettled = false
  );

  // FORK(hollowed-oath): fork-added API (see FlushDetachedCharacterData
  // above). Keep across upstream merges.
  //
  // Emit a (second) player-left that releases the backend's
  // character->instance write binding for a character whose logout-time
  // player-left carried retainBinding (URedwoodGameModeComponent's
  // ShouldRetainCharacterBinding gate). Call when the character actually
  // leaves the world. If a final flush must be processed first, do not
  // sequence it by emission order — same-socket delivery order does not
  // serialize the async backend handlers; use
  // FlushDetachedCharacterData(..., bReleaseBindingWhenSettled) which chains
  // this call on the flush acknowledgment.
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void EmitPlayerLeft(const FString &PlayerId, const FString &CharacterId);

  void InitialDataLoad(FRedwoodDelegate OnComplete);

  void RegisterSyncComponent(
    URedwoodSyncComponent *InComponent, bool bDelayNewSync
  );

  void PutBlob(
    const FString &Key,
    const TArray<uint8> &Value,
    FRedwoodErrorOutputDelegate OnComplete
  );
  void GetBlob(const FString &Key, FRedwoodGetBlobOutputDelegate OnComplete);

  // FORK(hollowed-oath): account documents (see
  // Types/RedwoodTypesAccountDocuments.h). Uses the backend, or
  // Saved/Persistence/AccountDocuments/<PlayerId>/<Key>.json when the backend
  // is off (PIE). The output has the same shape either way, but the offline
  // path calls OnComplete before the call returns, and the backend path calls
  // it later. A missing document is not an error: bFound is false. A put is
  // accepted only if the document is still at ExpectedRevision (0 = it must
  // not exist yet); otherwise the output has bConflict set. An error after the
  // send can mean the outcome is unknown, so a caller must Get before it
  // retries. The backend path has no timeout of its own: if the sidecar
  // socket drops, OnComplete may never be called, so callers must apply their
  // own timeout.
  void GetAccountDocument(
    const FString &PlayerId,
    const FString &Key,
    FRedwoodGetAccountDocumentOutputDelegate OnComplete
  );
  void PutAccountDocument(
    const FString &PlayerId,
    const FString &Key,
    TSharedPtr<FJsonObject> Data,
    int32 ExpectedRevision,
    FRedwoodPutAccountDocumentOutputDelegate OnComplete
  );

  void PutSaveGame(
    const FString &Key, USaveGame *Value, FRedwoodErrorOutputDelegate OnComplete
  );
  void GetSaveGame(
    const FString &Key, FRedwoodGetSaveGameOutputDelegate OnComplete
  );

  void GetPartyById(
    const FString &PartyId, FRedwoodGetPartyOutputDelegate OnOutput
  );
  void GetPartyByPlayerId(
    const FString &PlayerId, FRedwoodGetPartyOutputDelegate OnOutput
  );

  // FORK(hollowed-oath) BEGIN: the in-game /gm command. Ask the backend to
  // change the account role of a player, which lasts past the session because
  // the account service, not this server, holds it. Fork-added.
  //
  // RoleKey is a backend role name, or EMPTY to clear the role back to the
  // lowest tier. The plugin never maps a tier number to a key: the game sends
  // the key and reads the number back, so a new tier needs no plugin change.
  //
  // ActorPlayerId names the player who asked, and the backend applies its rank
  // rule to it. Take it ONLY from the authenticated session on this server.
  // A value that comes from anything a client sent would let any player claim
  // the rank of any other.
  //
  // OnOutput IS NOT GUARANTEED TO FIRE. There is no timeout on the answer, so
  // a sidecar that never answers -- an image without this route during a
  // rolling deploy, for one -- leaves the request open for the rest of the
  // session. A caller must not hold state that only this answer can release:
  // no latch, no "a change is in flight" flag that blocks the next command.
  // The delegate also fires INLINE, before this function returns, when the
  // sidecar is down; on every other path it fires later, from the socket.
  void RequestPlayerRoleChange(
    const FString &TargetPlayerId,
    const FString &RoleKey,
    const FString &ActorPlayerId,
    FRedwoodSetPlayerRoleOutputDelegate OnOutput
  );

  // Wire name of the fork-added role-change request. Named and pinned by a
  // test, because the backend pins its own copy of this string and the two
  // cannot share a constant: a silent drift on either side would turn the
  // whole command off with nothing to show for it.
  static constexpr const TCHAR *SetPlayerRoleEventName =
    TEXT("realm:servers:set-player-role:game-server-to-sidecar");

  // The two halves of RequestPlayerRoleChange that hold rules, kept apart from
  // the socket so the automation test can reach them without a backend.
  static TSharedPtr<FJsonObject> MakeSetPlayerRolePayload(
    const FString &TargetPlayerId,
    const FString &RoleKey,
    const FString &ActorPlayerId
  );
  static void AnswerSetPlayerRole(
    const TArray<TSharedPtr<FJsonValue>> &Response,
    FRedwoodSetPlayerRoleOutputDelegate OnOutput
  );

  // Ask the realm for every character in play on it, on any game server: the
  // game-server side of the in-game /who all command. Fork-added. The same
  // answer contract as RequestPlayerRoleChange: OnOutput is not guaranteed to
  // fire, and fires INLINE, with an error, when the sidecar is down.
  void RequestOnlineCharacters(
    FRedwoodListOnlineCharactersOutputDelegate OnOutput
  );

  // Wire name, pinned by a test against the backend's own copy of the string.
  static constexpr const TCHAR *ListOnlineCharactersEventName =
    TEXT("realm:servers:list-online-characters:game-server-to-sidecar");

  // The parse half of RequestOnlineCharacters, kept apart from the socket so
  // the automation test can reach it without a backend. An answer that is not
  // one of the backend's comes back with an error, never as an empty roster.
  static FRedwoodListOnlineCharactersOutput ParseListOnlineCharacters(
    const TArray<TSharedPtr<FJsonValue>> &Response
  );
  // FORK(hollowed-oath) END

  // The latest party data for all parties that have at least one
  // member connected to this server, keyed by party id. The realm
  // backend pushes updates as parties change; each push fully replaces
  // this map.
  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  TMap<FString, FRedwoodParty> TrackedParties;

  // Broadcast after the realm backend pushes new party data into
  // TrackedParties (and the PlayerStateComponents' PartyIds have been
  // updated to match).
  UPROPERTY(BlueprintAssignable, Category = "Redwood")
  FRedwoodDynamicDelegate OnTrackedPartiesUpdated;

  // Returns the tracked party with the given id; bValid is false if
  // this server doesn't currently track it.
  UFUNCTION(BlueprintPure, Category = "Redwood")
  FRedwoodParty GetTrackedPartyById(const FString &InPartyId) const;

  // Returns the tracked party that has a member with the given player
  // id; bValid is false if there isn't one.
  UFUNCTION(BlueprintPure, Category = "Redwood")
  FRedwoodParty GetTrackedPartyByPlayerId(const FString &InPlayerId) const;

  // Reapplies TrackedParties to the PartyId of every
  // URedwoodPlayerStateComponent in the current world. Called
  // automatically when the backend pushes party data and when a player
  // finishes authentication.
  void UpdatePlayerStateComponentPartyIds();

  // True if any player connected to this server is a member of the
  // given party. Used to decide whether a single-party update should
  // be tracked or dropped.
  bool DoesServerHostPartyMember(const FRedwoodParty &Party) const;

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void RequestEngineExit(bool bForce);

private:
  TMap<FString, TSubclassOf<AGameModeBase>> GameModeClasses;
  TMap<FString, FPrimaryAssetId> Maps;
  TMap<FString, TWeakObjectPtr<URedwoodSyncItemAsset>> SyncItemTypesByTypeId;
  TMap<FString, TWeakObjectPtr<URedwoodSyncItemAsset>>
    SyncItemTypesByPrimaryAssetId;
  TMap<FString, TWeakObjectPtr<URedwoodSyncComponent>> SyncItemComponentsById;

  void InitializeSidecar();
  void SendUpdateToSidecar();

  // FORK(hollowed-oath): the one kick that a failed transfer uses, shared by
  // the ambiguous answer and the answer that never came. Says so in the log
  // when this server has no game session to kick with, because then the
  // player stays here and stays marked as transferring.
  void KickPlayerAfterFailedTransfer(
    APlayerController *PlayerController, const FString &Error
  );

  // FORK(hollowed-oath): the shared tail of both TravelPlayerToZone*
  // functions: the wait for the sidecar's answer, then the emit that asks
  // for the transfer. Upstream wrote the emit out in each travel function;
  // the fork added the wait and the answer rules to it. Both live here so
  // each upstream function keeps one fork line, which is the smallest fork
  // footprint in upstream code. See TransferAnswerTimeoutSeconds,
  // HandleTransferAnswerTimeout, and HandleTransferZoneResponse above.
  void EmitTransferZoneRequest(
    URedwoodPlayerStateComponent *PlayerStateComponent,
    APlayerController *PlayerController,
    const TSharedPtr<FJsonObject> &Payload
  );

  void GetParty(
    const FString &PartyId,
    const FString &PlayerId,
    FRedwoodGetPartyOutputDelegate OnOutput
  );

  TSharedPtr<FSocketIONative> Sidecar;

  // FORK(hollowed-oath) BEGIN: the currency reload that held item flushes wait on, keyed by
  // character id. While any component of a character needs a rebase (a trade used its seq), no
  // flush of that character goes out, items included: an item row must not commit before the
  // debit that pays for it. Its reply rebases the held entries and resumes every held attempt,
  // settle barrier and all.
  TMap<FString, FRedwoodCurrencyReload> CurrencyReloads;
  uint64 LastCurrencyReloadGeneration = 0;

  // Characters frozen by a final refusal (see IsCharacterFrozen).
  TSet<FString> FrozenCharacterIds;

  // Freezes the character: every component that flushes from now on drops what it has staged.
  // Ends any reload of the character; its held attempts settle with nothing left to save.
  void FreezeCharacter(
    const FString &CharacterId, URedwoodPlayerStateComponent *PlayerStateComponent
  );

  // The sidecar's connect event: the upstream update timers, then the reloads that waited.
  void BindSidecarConnectedCallback();
  void HandleSidecarConnected();

  // Joins the reload of this character, or starts one. OnAckSettled (nullable) is the held flush
  // attempt's settle; the reload resumes the flush with it.
  void HoldItemFlushForCurrencyReload(
    const FString &CharacterId,
    URedwoodPlayerStateComponent *PlayerStateComponent,
    URedwoodCharacterComponent *CharacterComponent,
    TFunction<void()> OnAckSettled
  );
  void SendCurrencyReload(const FString &CharacterId);
  void HandleCurrencyReloadReply(
    const FString &CharacterId, uint64 Generation, const TSharedPtr<FJsonObject> &Reply
  );
  void HandleCurrencyReloadTimeout(const FString &CharacterId, uint64 Generation);
  void RetryCurrencyReload(const FString &CharacterId);
  // Sends every reload that waited for the sidecar. Run from the sidecar's connect event.
  void ResumeCurrencyReloadsAfterSidecarConnect();
  // Ends a reload that cannot land: logs every entry it leaves unsaved, so staff can rebuild it,
  // then settles every held attempt. The currency stays held on the components.
  void GiveUpCurrencyReload(const FString &CharacterId, const FString &Reason);
  // Ends the reload: clears its timer and settles every held attempt without resuming it.
  void AbandonCurrencyReload(const FString &CharacterId);
  // Runs every held attempt again, with its settle; one whose component is gone just settles.
  void ResumeHeldItemFlushes(TArray<FRedwoodHeldItemFlush> &HeldFlushes);

  // Trades that went out and have not settled, keyed by trade id (see EmitItemsTrade).
  TMap<FString, FRedwoodPendingItemTrade> PendingItemTrades;
  // Every trade id this server has sent. A reused id is refused (see EmitItemsTrade).
  TSet<FString> SentItemTradeIds;
  uint64 LastItemTradeGeneration = 0;

  // Item flushes sent and not yet answered, per character, over every component of it. A trade must
  // not start while one is open: that batch would commit before or after the trade, so the seq and
  // the balance the trade starts from would be guesses. A reply that never comes leaves the count
  // up, as it leaves the component's own flight slot claimed.
  TMap<FString, int32> InFlightItemFlushesByCharacter;
  void EndItemFlushInFlight(const FString &CharacterId);

  // The unsettled trade that holds this character, or null.
  FRedwoodPendingItemTrade *FindItemTradeOfCharacter(const FString &CharacterId);
  // Fills one side from its component and snapshot. False, with OutError, when the character
  // must not trade now.
  bool PrepareItemTradeSide(
    URedwoodCharacterComponent *CharacterComponent,
    FRedwoodItemTradeSide &OutSide,
    FString &OutError
  );
  void SendItemsTrade(const FString &TradeId);
  void HandleItemsTradeReply(
    const FString &TradeId, uint64 Generation, const TSharedPtr<FJsonObject> &Reply
  );
  void HandleItemsTradeTimeout(const FString &TradeId, uint64 Generation);
  // After an attempt whose outcome is unknown, or that did not reach the realm.
  void RetryItemsTrade(const FString &TradeId, const FString &Reason);
  void ResumeItemsTradesAfterSidecarConnect();
  // A committed trade: each side adopts its fence and balance.
  void CommitItemsTrade(const FString &TradeId, const FRedwoodItemTradeReply &Reply);
  // A trade whose outcome cannot be learned: logs it for staff and freezes both characters.
  void GiveUpItemsTrade(const FString &TradeId, const FString &Reason);
  // Ends the trade: tells the caller, then resumes every held flush.
  void FinishItemsTrade(
    const FString &TradeId, const FString &Error, int64 FromCommittedSeq, int64 ToCommittedSeq
  );

  friend class FRedwoodCurrencyFlushTestAccess;
  // FORK(hollowed-oath) END

  float UpdateSidecarRate = 3.f; // in seconds
  float UpdateSidecarLoadingRate = 0.2f; // in seconds
  FTimerHandle TimerHandle_UpdateSidecar;
  FTimerHandle TimerHandle_UpdateSidecarLoading;

  bool bIsShuttingDown = false;
  FGameplayMessageListenerHandle ListenerHandle;
  void OnShutdownMessage(FGameplayTag InChannel, const FRedwoodReason &Message);

  TSet<TWeakObjectPtr<URedwoodSyncComponent>> DelayedNewSyncItems;
  bool bInitialDataLoaded = false;
  FRedwoodDelegate InitialDataLoadCompleteDelegate;
  void PostInitialDataLoad(TSharedPtr<FJsonObject> ZoneJsonObject);

  void UpdateSyncItem(FRedwoodSyncItem &Item);
  void UpdateSyncItemState(
    URedwoodSyncComponent *SyncItemComponent, FRedwoodSyncItemState &ItemState
  );
  void UpdateSyncItemMovement(
    URedwoodSyncComponent *SyncItemComponent,
    FRedwoodSyncItemMovement &ItemMovement
  );
  void UpdateSyncItemData(
    URedwoodSyncComponent *SyncItemComponent, USIOJsonObject *InData
  );

  void SendNewSyncItemToSidecar(URedwoodSyncComponent *InComponent);
  void SendNewSyncForPersistentItemsToSidecar();
};
