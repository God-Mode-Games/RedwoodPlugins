// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "Types/RedwoodTypes.h"

#include "RedwoodCharacterComponent.generated.h"

class USIOJsonObject;

UCLASS(
  Blueprintable,
  BlueprintType,
  ClassGroup = (Redwood),
  meta = (BlueprintSpawnableComponent)
)
class REDWOOD_API URedwoodCharacterComponent : public UActorComponent {
  GENERATED_BODY()

public:
  URedwoodCharacterComponent(const FObjectInitializer &ObjectInitializer);

  //~ Begin UActorComponent Interface
  virtual void BeginPlay() override;
  //~ End UActorComponent Interface

  UPROPERTY(BlueprintAssignable, Category = "Redwood")
  FRedwoodDynamicDelegate OnRedwoodCharacterUpdated;

  UPROPERTY(BlueprintAssignable, Category = "Redwood")
  FRedwoodDynamicDelegate OnRedwoodPlayerUpdated;

  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Redwood")
  bool bStoreDataInActor = true;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FString RedwoodPlayerId;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FString RedwoodPlayerNickname;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FString RedwoodNameTag;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  bool bSelectedGuildValid = false;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FRedwoodGuildInfo SelectedGuild;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FString RedwoodCharacterId;

  UPROPERTY(Replicated, BlueprintReadOnly, Category = "Redwood")
  FString RedwoodCharacterName;

  // PlayerData is not the same as the other character data in
  // below variables. This PlayerData is associated with the
  // PlayerIdentity, not the PlayerCharacter. This means that
  // it is the same for all characters of a player across all
  // realms. It's disabled by default as most developers likely
  // won't use it.
  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUsePlayerData = false;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString PlayerDataVariableName = TEXT("PlayerData");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestPlayerDataSchemaVersion = 0;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseCharacterCreatorData = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString CharacterCreatorDataVariableName = TEXT("CharacterCreatorData");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestCharacterCreatorDataSchemaVersion = 0;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseMetadata = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString MetadataVariableName = TEXT("Metadata");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestMetadataSchemaVersion = 0;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseEquippedInventory = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString EquippedInventoryVariableName = TEXT("EquippedInventory");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestEquippedInventorySchemaVersion = 0;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseNonequippedInventory = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString NonequippedInventoryVariableName = TEXT("NonequippedInventory");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestNonequippedInventorySchemaVersion = 0;

  // FORK(hollowed-oath) BEGIN: per-item-persistence config knobs. Fork-added; no upstream
  // counterpart. bUseItems gates the whole channel; ItemsVariableName names the game's
  // TArray<FRedwoodItemRecord> UPROPERTY that ResolveItemsRecordsArray reflects into.
  // Merge must preserve both as EditAnywhere config (games override ItemsVariableName).
  // Per-item persistence channel (the backend Item table). Unlike the channels above,
  // this is not a single whole-blob field: ItemsVariableName names a
  // TArray<FRedwoodItemRecord> UPROPERTY on the owning actor, and only the DIRTY records
  // (MarkItemsDirty) plus any pending deletions (MarkItemsDeleted) are sent to the
  // realm:characters:items:flush sidecar route on flush -- so a change to one item doesn't
  // resend every other item. LOADING, unlike flushing, rides the SAME round trip as the rest
  // of the character (FRedwoodCharacterBackend::Items, populated by the player-auth /
  // character-load response) rather than its own route -- see
  // RedwoodPlayerStateCharacterUpdated(), which copies it into ItemsVariableName before
  // broadcasting OnRedwoodCharacterUpdated.
  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseItems = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString ItemsVariableName = TEXT("Items");
  // FORK(hollowed-oath) END

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseProgress = false;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString ProgressVariableName = TEXT("Progress");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestProgressSchemaVersion = 0;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseData = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString DataVariableName = TEXT("Data");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestDataSchemaVersion = 0;

  // Only enable this if you're using a custom Ability System;
  // if you're using GAS, keep this false and use the URedwoodGASComponent
  // instead.
  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseAbilitySystem = false;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString AbilitySystemVariableName = TEXT("AbilitySystem");

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  int32 LatestAbilitySystemSchemaVersion = 0;

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkPlayerDataDirty() {
    if (bUsePlayerData) {
      bPlayerDataDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkCharacterCreatorDataDirty() {
    if (bUseCharacterCreatorData) {
      bCharacterCreatorDataDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkMetadataDirty() {
    if (bUseMetadata) {
      bMetadataDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkEquippedInventoryDirty() {
    if (bUseEquippedInventory) {
      bEquippedInventoryDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkNonequippedInventoryDirty() {
    if (bUseNonequippedInventory) {
      bNonequippedInventoryDirty = true;
    }
  }

  // FORK(hollowed-oath) BEGIN: item dirty/delete-marking API. Fork-added; the game module
  // (AClient / per-bag inventory) calls MarkItemsDirty / MarkItemsDeleted to drive the
  // per-item persistence channel. Merge must preserve the delete-cancel-on-redirty behavior
  // (a re-dirty removes a pending deletion, and vice versa) and the per-id GENERATION counter that
  // CompleteItemFlush relies on -- these are the correctness core of the whole channel.
  // Records item ids whose game-side record changed since the last flush. Dirty ids
  // accumulate (a second call before a flush unions in, it doesn't replace) so nothing is lost
  // if multiple mutations land in the same tick. Each id carries a GENERATION counter (bumped on
  // every mark) rather than a plain presence flag -- see CompleteItemFlush for why: it lets an
  // ack tell "the id I sent" apart from "the id as it stands after a re-dirty that arrived while
  // that send was still in flight".
  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkItemsDirty(const TArray<FString> &DirtyItemIds) {
    if (bUseItems) {
      for (const FString &Id : DirtyItemIds) {
        ++DirtyItemGenerations.FindOrAdd(Id);
        // A re-dirtied id cancels any pending deletion of the same row (delete-then-recreate,
        // e.g. an item removed then re-created inside one flush window). Without this, one
        // flush sends the id as BOTH upsert and delete, and the backend transaction deletes
        // last -- the recreated row is lost while both acks report success. Correct whether or
        // not a delete is already in flight: an in-flight delete's ack then no-ops on the
        // removed entry and the surviving dirty upsert recreates the row.
        PendingDeletedItemGenerations.Remove(Id);
      }
    }
  }

  // Records item ids whose row should be DELETED on next flush (e.g. an item that got
  // consumed/discarded). A deleted id is dropped from the dirty-upsert set too -- there's no point
  // upserting a row the same flush is about to delete. Same generation-counter treatment as
  // MarkItemsDirty, for the same reason.
  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkItemsDeleted(const TArray<FString> &DeletedItemIds) {
    if (bUseItems) {
      for (const FString &Id : DeletedItemIds) {
        ++PendingDeletedItemGenerations.FindOrAdd(Id);
        DirtyItemGenerations.Remove(Id);
      }
    }
  }
  // FORK(hollowed-oath) END

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkProgressDirty() {
    if (bUseProgress) {
      bProgressDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkDataDirty() {
    if (bUseData) {
      bDataDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkAbilitySystemDirty() {
    if (bUseAbilitySystem) {
      bAbilitySystemDirty = true;
    }
  }

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  bool IsPlayerDataDirty() const {
    return bPlayerDataDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsCharacterCreatorDataDirty() const {
    return bCharacterCreatorDataDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsMetadataDirty() const {
    return bMetadataDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsEquippedInventoryDirty() const {
    return bEquippedInventoryDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsNonequippedInventoryDirty() const {
    return bNonequippedInventoryDirty;
  }

  // FORK(hollowed-oath) BEGIN: item dirty-state query, drain accessors, the two clear paths, and
  // the protocol-v2 single-flight/batchSeq API. Fork-added. The critical invariant an upstream
  // merge MUST keep: CompleteItemFlush clears an id only if its generation still matches what was
  // sent (so a re-dirty racing an in-flight send is never silently dropped), whereas
  // ClearItemsDirtyState is the unconditional clear reserved for paths with no in-flight send to
  // race. Consumed by URedwoodServerGameSubsystem's item flush / offline append paths.
  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsItemsDirty() const {
    return DirtyItemGenerations.Num() > 0 || PendingDeletedItemGenerations.Num() > 0;
  }

  // Drained by the server item-flush path when building the upsert payload; not BlueprintCallable
  // (Blueprint has no TMap<FString, uint64> pin type). The value is the dirty GENERATION for that
  // id -- see CompleteItemFlush.
  const TMap<FString, uint64> &GetDirtyItemGenerations() const {
    return DirtyItemGenerations;
  }

  const TMap<FString, uint64> &GetPendingDeletedItemGenerations() const {
    return PendingDeletedItemGenerations;
  }

  // Unconditional full clear -- used where there is genuinely nothing to retry (e.g. the
  // identity-mismatch safety rail that skips the flush entirely for a stale/un-hydrated component,
  // so there is no in-flight send whose ack this would race). The normal flush path does NOT call
  // this -- see CompleteItemFlush below, which clears only the specific ids a successfully-
  // acknowledged flush actually sent.
  void ClearItemsDirtyState() {
    DirtyItemGenerations.Empty();
    PendingDeletedItemGenerations.Empty();
  }

  // Protocol-v2 single-flight/batchSeq accessors. See the FORK rationale block on the state
  // members (bItemFlushInFlight / NextBatchSeq) for WHY the item channel is single-flight and
  // sequence-fenced where the sibling channels are not.
  bool IsItemFlushInFlight() const {
    return bItemFlushInFlight;
  }

  int64 GetLastCommittedItemSeq() const {
    return LastCommittedItemSeq;
  }

  // Claims the single flight slot and the next batchSeq for a flush the server is about to send.
  // Returns false (and leaves OutBatchSeq untouched) if a flush is already in flight -- the caller
  // must not send a second, overlapping item batch (see the state-member FORK block). On success,
  // OutBatchSeq is the NextBatchSeq value the flush must stamp on its payload, and that same value
  // is remembered in InFlightBatchSeq so CompleteItemFlush can recognise a backend fence at or
  // past the seq we sent without being handed it explicitly.
  bool TryBeginItemFlush(int64 &OutBatchSeq);

  // Ack handler for the in-flight item flush. On success (Error empty) clears the SENT generations
  // (generation-matched, exactly as the old container ack did) and advances the committed seq. On
  // error it normally leaves dirt in place so the next tick re-sends -- EXCEPT when the backend
  // reports it has already committed at or past the seq we sent (replay/fence resync), in which
  // case NextBatchSeq is realigned to CommittedSeq + 1 so future flushes aren't wedged behind a
  // gap the backend will keep rejecting. ALWAYS releases the flight slot. SentUpserts/SentDeletes
  // pair each sent id with the GENERATION current for it at send time.
  void CompleteItemFlush(
    const FString &Error,
    int64 CommittedSeq,
    const TArray<TPair<FString, uint64>> &SentUpserts,
    const TArray<TPair<FString, uint64>> &SentDeletes
  );

  // Seeds the committed-seq/next-batchSeq state from a freshly-loaded character's InventorySeq.
  // Called from the load-broadcast copy site (RedwoodPlayerStateCharacterUpdated) so the first
  // flush of the session continues the backend's sequence instead of restarting at 1.
  void SeedItemSeqFromCharacter(int64 InventorySeq);
  // FORK(hollowed-oath) END

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsProgressDirty() const {
    return bProgressDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsDataDirty() const {
    return bDataDirty;
  }

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsAbilitySystemDirty() const {
    return bAbilitySystemDirty;
  }

  // FORK(hollowed-oath): the item dirty-state carve-out below is a fork addition to an
  // otherwise-upstream method. An upstream merge MUST keep ClearDirtyFlags from clearing
  // DirtyItemGenerations / PendingDeletedItemGenerations -- those clear only on ack.
  // NOTE: deliberately does NOT touch the item dirty state (DirtyItemGenerations /
  // PendingDeletedItemGenerations) -- unlike every flag cleared below, items ride their own
  // acknowledged, sequence-fenced send (CompleteItemFlush), so clearing them here (before the ack)
  // would drop a flush that later fails or never arrives.
  void ClearDirtyFlags() {
    bPlayerDataDirty = false;
    bCharacterCreatorDataDirty = false;
    bMetadataDirty = false;
    bEquippedInventoryDirty = false;
    bNonequippedInventoryDirty = false;
    bProgressDirty = false;
    bDataDirty = false;
    bAbilitySystemDirty = false;
  }

private:
  UFUNCTION(BlueprintCallable)
  void OnControllerChanged(
    APawn *Pawn, AController *OldController, AController *NewController
  );

  UFUNCTION()
  void RedwoodPlayerStatePlayerUpdated();

  UFUNCTION()
  void RedwoodPlayerStateCharacterUpdated();

  // Bumped on the server (authority) after the player/character fields above
  // have been copied from the server-only PlayerStateComponent structs. The
  // ReplicatedUsing notifies fire on clients once the new value (and the
  // accompanying field updates in the same net update) have been applied,
  // which is how clients learn to broadcast OnRedwoodPlayerUpdated /
  // OnRedwoodCharacterUpdated. This replaces the old MC_* multicast RPCs so
  // that late-relevant clients are notified too.
  UPROPERTY(ReplicatedUsing = OnRep_RedwoodPlayerUpdated)
  uint8 RedwoodPlayerUpdateCount = 0;

  UPROPERTY(ReplicatedUsing = OnRep_RedwoodCharacterUpdated)
  uint8 RedwoodCharacterUpdateCount = 0;

  UFUNCTION()
  void OnRep_RedwoodPlayerUpdated();

  UFUNCTION()
  void OnRep_RedwoodCharacterUpdated();

  bool bPlayerDataDirty = false;
  bool bCharacterCreatorDataDirty = false;
  bool bMetadataDirty = false;
  bool bEquippedInventoryDirty = false;
  bool bNonequippedInventoryDirty = false;
  bool bProgressDirty = false;
  bool bDataDirty = false;
  bool bAbilitySystemDirty = false;

  // FORK(hollowed-oath) BEGIN: fork-added backing state for the per-item persistence channel.
  // Value is a per-id dirty GENERATION, not a plain membership flag -- see CompleteItemFlush.
  TMap<FString, uint64> DirtyItemGenerations;
  TMap<FString, uint64> PendingDeletedItemGenerations;

  // Protocol-v2 single-flight + sequence-fence state. WHY single-flight exists: the item channel
  // has no tombstones and the backend enforces strict ordering on the per-character mutation
  // sequence -- it REJECTS a batch whose seq leaves a GAP (a seq ahead of what it has committed)
  // and NO-OPS a replay (a seq at or below what it has already committed). If two flushes were in
  // flight at once their acks could interleave and either wedge the channel behind a perceived gap
  // or silently drop the later batch as a replay. So exactly one item flush may be outstanding:
  // TryBeginItemFlush claims bItemFlushInFlight and stamps the batch with NextBatchSeq (remembered
  // in InFlightBatchSeq), and CompleteItemFlush releases the slot, advancing the seq only on a
  // confirmed commit or a backend-reported fence. LastCommittedItemSeq mirrors the backend's last
  // committed seq (seeded from the loaded character's InventorySeq); NextBatchSeq is the seq the
  // next flush will claim.
  bool bItemFlushInFlight = false;
  int64 LastCommittedItemSeq = 0;
  int64 NextBatchSeq = 1;
  // The batchSeq handed out by the current in-flight TryBeginItemFlush; lets CompleteItemFlush
  // detect a backend fence at or past what we sent without the seq being passed back in.
  int64 InFlightBatchSeq = 0;

  // The character whose login snapshot has already been hydrated into the game's item array. The
  // hydration is INITIAL-LOAD-ONLY: RedwoodPlayerStateCharacterUpdated re-runs on every
  // OnControllerChanged, and with linkdead pawn retention (#1365) a LIVE pawn can be re-possessed
  // after mutations have already advanced the wire array past that snapshot — re-applying it there
  // reverts moves/quantities and can strand dirty ids with no matching row. Empty until the first
  // hydration; compared against the incoming character id so a genuinely different character (a
  // component reused across characters) still hydrates.
  FString HydratedItemsCharacterId;
  // FORK(hollowed-oath) END
};
