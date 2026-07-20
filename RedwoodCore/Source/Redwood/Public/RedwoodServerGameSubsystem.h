// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodGameplayTags.h"
#include "RedwoodModule.h"
#include "Types/RedwoodTypes.h"

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
// FORK(hollowed-oath): forward decls for the item flush / migrate / trade helpers' parameters
// (fork-added).
class URedwoodCharacterComponent;
class URedwoodPlayerStateComponent;

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
  // feat/item-persistence line (realm:characters:items:{flush,migrate,trade} routes). Preserve
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

  // One-shot emit of realm:characters:items:migrate: hands the backend a whole set of item rows to
  // fold into the Item table (the game's one-time legacy-inventory backfill, Plan C). Does NOT
  // touch the component's dirty/flush state -- the rows are supplied whole by the caller, not
  // drained from the dirty maps. OnComplete reports the backend's error string and whether the
  // character was already migrated (alreadyMigrated true = a prior migrate already ran; the caller
  // should treat that as success and not re-backfill).
  void EmitItemsMigrate(
    URedwoodPlayerStateComponent *PlayerStateComponent,
    const TArray<FRedwoodItemRecord> &Items,
    TFunction<void(FString Error, bool bAlreadyMigrated)> OnComplete
  );

  // Emit realm:characters:items:trade: atomically moves the given root items (each identified by a
  // (Domain, Slot) placement, optionally with a ParentId for a content-child destination -- see
  // FRedwoodTradeRootPlacement) from one character to another on the backend. RootPlacements name
  // only the root rows being handed over; the backend re-parents each root and its nested children
  // in one transaction. After a successful trade the backend bumps BOTH characters' InventorySeq,
  // so each caller's next FlushItemsForCharacterComponent takes the seq-resync path (CommittedSeq
  // ahead of its sent seq) and realigns -- the trade itself does not flush either side here.
  //
  // FORK(hollowed-oath): FromCommittedSeq/ToCommittedSeq are the post-trade InventorySeq fences the
  // backend returns on the ack (-1 on error paths; see RedwoodBackend feat/item-persistence trade
  // handler). The backend bumps BOTH characters' seq server-side as part of the trade transaction,
  // so each character's *next* flush arrives one seq behind what the server now expects -- without
  // intervention that flush is silently treated as an idempotent replay (its batchSeq no longer
  // matches CommittedSeq) even though the ack it receives is success-shaped, which clears the
  // client's dirty state and DROPS the flush's contents. The CALLER (the game's trade task, which
  // is the only place both characters' URedwoodCharacterComponent are reachable) MUST call
  // SeedItemSeqFromCharacter on each trading character's component with the matching fence value
  // BEFORE releasing that character's flush lane, or this loss reproduces on every trade. This
  // function does not perform that re-seed itself -- the contract is documented here, on the
  // signature, deliberately not wired in, because the components live outside RedwoodCore's reach
  // from this call site.
  void EmitItemsTrade(
    const FString &FromCharacterId,
    const FString &ToCharacterId,
    const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
    TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete
  );
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

  void GetParty(
    const FString &PartyId,
    const FString &PlayerId,
    FRedwoodGetPartyOutputDelegate OnOutput
  );

  TSharedPtr<FSocketIONative> Sidecar;

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
