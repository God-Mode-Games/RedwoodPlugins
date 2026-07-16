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
