// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "Types/RedwoodTypes.h"
// FORK(hollowed-oath): currency entries ride the item flush.
#include "RedwoodCurrencyTypes.h"

#include "RedwoodCharacterComponent.generated.h"

class USIOJsonObject;
// FORK(hollowed-oath): the snapshot owner FindPlayerStateComponent returns.
class URedwoodPlayerStateComponent;

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
        // Stamp from ONE monotonic counter, never a per-id ++ on FindOrAdd. A per-id counter
        // restarts at 1 whenever the id's entry has been removed -- which is exactly what a
        // remove-then-recreate does (MarkItemsDeleted drops the dirty entry, this call re-adds
        // it). An upsert ack still in flight from before that cycle carries generation 1 too, so
        // it matched the RECREATED entry and cleared it: the recreation was never flushed and the
        // backend kept the stale pre-removal row. A shared counter makes a new stamp strictly
        // greater than anything ever sent, so a stale ack can never match it.
        DirtyItemGenerations.Add(Id, ++NextItemGeneration);
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
        // Same shared monotonic counter as MarkItemsDirty, and for the same reason -- generations
        // must not restart when an id moves between the dirty and pending-delete maps.
        PendingDeletedItemGenerations.Add(Id, ++NextItemGeneration);
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
    // FORK(hollowed-oath): staged currency counts as dirt, so a balance change with no item change
    // still drives a flush.
    return DirtyItemGenerations.Num() > 0 || PendingDeletedItemGenerations.Num() > 0 ||
      PendingCurrencyEntries.Num() > 0 || PendingGrantKeys.Num() > 0;
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

  // FORK(hollowed-oath) BEGIN: currency ledger staging. Entries and one-time grant keys wait here
  // until a flush carries them; they leave the queue only on an ack that proves the backend has
  // them (success with a trustworthy seq) or on an explicit final refusal. Any other error keeps
  // them -- an error reply means NOT saved, whatever committedSeq says.
  //
  // Two rules keep an amount from being lost or applied twice when an ack is lost:
  //  - Unconfirmed batch: after any error except the two below (a timeout, a dropped socket, a
  //    "concurrent flush" refusal -- whatever committedSeq says), the next batch carries EXACTLY the same entries and keys under the SAME
  //    seq. The backend then sees a replay and acks it from its ledger (all ids present) or refuses
  //    it retriably (none present). Adding a newer entry to that resend would turn a committed
  //    replay into a mixed batch that the backend refuses, and the recovery below would then
  //    re-chain the committed entry on top of itself.
  //  - Rebase: after an error with `rebaseRequired` (the backend's replay-mismatch refusal: a trade
  //    used the seq and the batch did not apply), the backend balance is not what the pending
  //    entries were chained from. No currency is sent until the caller reloads the balance and
  //    calls RebaseCurrency.
  // A final refusal is different: the game and the backend disagree, and nothing here can tell
  // which staged change depended on the refused one. The character freezes (see
  // FreezeAfterFinalRefusal) until it is hydrated again from the database.
  //
  // Returns false when anything was refused: every entry and key while the character is frozen or
  // its writer fence is lost, or an entry whose id the backend would refuse. The caller must not count a refused amount.
  // FORK(hollowed-oath): also a debit that would go below zero when an unsettled trade of this
  // character settles (URedwoodServerGameSubsystem::GetUnsettledTradeFloorBalance).
  bool QueueCurrency(
    const TArray<FRedwoodCurrencyEntry> &Entries, const TArray<FString> &GrantKeys
  );

  const TArray<FRedwoodCurrencyEntry> &GetPendingCurrencyEntries() const {
    return PendingCurrencyEntries;
  }

  const TArray<FString> &GetPendingGrantKeys() const {
    return PendingGrantKeys;
  }

  // Call before staging a one-time reward. True when the key is already used: committed (on the
  // PlayerState snapshot), or staged, in flight or unconfirmed on this component (a key leaves the
  // queue only when its batch commits). Case-sensitive, as the backend is. Staging a used key gets
  // the whole batch refused for good, which freezes the character. Keys staged on ANOTHER
  // component of the same character are not seen: stage rewards on one component.
  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool HasGrantKey(const FString &GrantKey) const;

  // Offline/PIE only: moves every staged entry and key out, for the save that writes them into the
  // character file. That save is synchronous and writes the whole file, so nothing is in flight or
  // unconfirmed and no ack will come: without this the queues would keep the component dirty for
  // the whole session.
  void TakePendingCurrencyForOfflineSave(
    TArray<FRedwoodCurrencyEntry> &OutEntries, TArray<FString> &OutGrantKeys
  ) {
    OutEntries = MoveTemp(PendingCurrencyEntries);
    OutGrantKeys = MoveTemp(PendingGrantKeys);
    PendingCurrencyEntries.Reset();
    PendingGrantKeys.Reset();
  }

  // How many entries / keys from the FRONT of the queues the next batch must carry. The flush
  // snapshots exactly this many at claim time and passes the same counts to CompleteItemFlush.
  // Zero while a rebase is required; exactly the unconfirmed batch while one is outstanding.
  int32 GetCurrencyEntriesToSendCount() const;
  int32 GetGrantKeysToSendCount() const;

  // Read right after TryBeginItemFlush. True when the claimed batch re-sends an unconfirmed
  // currency batch. Such a batch must carry NO item rows, at the old seq or not: if the original
  // committed, the backend answers the re-send as a replay and applies none of its rows, yet the
  // success ack would clear them -- rows dirtied since (a second purchase's item) would be lost.
  bool IsItemFlushReplayProbe() const {
    return bItemFlushInFlight && bInFlightIsCurrencyResend;
  }

  bool IsCurrencyRebaseRequired() const {
    return bCurrencyRebaseRequired;
  }

  // An earlier batch went out and its outcome is not known yet: it is re-sent exactly as it was.
  // Until its reply, nobody knows the backend balance, so a trade must wait.
  bool IsCurrencyBatchUnconfirmed() const {
    return bCurrencyBatchUnconfirmed;
  }

  // A final refusal froze this component: the backend refused a batch for good, so the game's
  // staged state no longer matches the backend's. Cleared only by a fresh hydration.
  bool IsFrozenAfterFinalRefusal() const {
    return bFrozenAfterFinalRefusal;
  }

  // The realm said this session's writer token is stale (`writerFenceLost`): another session owns
  // the character now. This component never sends again. Nothing is dropped: what it staged is
  // not this session's to throw away, and the owning session has the truth.
  bool IsWriterFenceLost() const {
    return bWriterFenceLost;
  }

  // Logs a warning the first time only.
  void MarkWriterFenceLost();

  // Drops EVERYTHING staged: dirty rows, pending deletes, entries and grant keys, sent or not,
  // and logs every dropped entry at Error so staff can rebuild it. An entry of an unconfirmed
  // batch is logged as "outcome unknown": it may be saved. Never call it while a batch is in
  // flight (except for the refused batch itself): that batch's reply decides its entries.
  // Repair is not attempted: a
  // staged change may depend on the refused one (a sale of a refused purchase, whose credit would
  // otherwise commit from nothing), and only the database knows the truth. The game sends the
  // player back through character select, which hydrates again and clears the freeze.
  void FreezeAfterFinalRefusal(const FString &Reason);

  // Holds currency until a reload rebases it: the balance the pending entries were chained from
  // is not known to be the backend's. Ignored while a batch is in flight or unconfirmed: a reload
  // could land before or after that batch commits, so its balance could count the batch's entries
  // once or twice, and that batch must go out again unchanged anyway; its reply decides.
  // bBeforeSettlingReply is for a caller about to hand CompleteItemFlush a success that settles
  // that batch, so the hold is in place before the parked attempt CompleteItemFlush runs.
  void RequireCurrencyReload(bool bBeforeSettlingReply = false);

  // Re-chains every pending entry's BalanceAfter from Balance (the amount just loaded from the
  // backend) and allows currency to be sent again. Ignored unless a rebase is required: at any
  // other time a pending entry may already be committed, and re-chaining it would count it twice.
  void RebaseCurrency(int64 Balance);

  // A trade of this character settled, and Balance is the backend balance after it: its own
  // entries' last BalanceAfter when it committed, the balance before it when it did not apply.
  // Every entry staged during the trade comes after it on the backend, so it is re-chained from
  // Balance, and the game is told (OnCurrencyRebased): the game cannot know which balance its
  // running total should have used while the outcome was open. Nothing can be in flight here,
  // because the trade held this character's flushes. If a re-chained entry would overflow or go
  // below zero, nothing is re-chained and the queue waits for a reload instead (see RebaseCurrency).
  void RechainCurrencyAfterTrade(int64 Balance);

  // Moves the committed seq and NextBatchSeq forward to CommittedSeq, never back: a replay probe
  // goes out under an older seq than NextBatchSeq, and its ack must not rewind the lane. Public for
  // the trade, whose reply is the other thing that moves this character's seq.
  void AdoptCommittedItemSeq(int64 CommittedSeq);

  // The PlayerState component that holds this character's snapshot: through the pawn's
  // controller, or the owner itself when this component sits on the PlayerState. Null for a
  // detached pawn.
  URedwoodPlayerStateComponent *FindPlayerStateComponent() const;

  // The token the realm issued this session at auth. Every items:flush must carry it; the backend
  // commits nothing for any other token.
  const FString &GetWriterToken() const {
    return WriterToken;
  }

  // Called at hydration, beside SeedItemSeqFromCharacter.
  void SeedWriterToken(const FString &InWriterToken) {
    WriterToken = InWriterToken;
  }

  FOnItemFlushRefusedFinal OnItemFlushRefusedFinal;
  FOnCurrencyAlerts OnCurrencyAlerts;
  FOnCurrencyRebased OnCurrencyRebased;
  FOnWriterFenceLost OnWriterFenceLost;
  // FORK(hollowed-oath) END

  // Ack handler for the in-flight item flush. On success (Error empty) clears the SENT generations
  // (generation-matched, exactly as the old container ack did) and advances the committed seq. On
  // error it normally leaves dirt in place so the next tick re-sends -- EXCEPT when the backend
  // reports it has already committed at or past the seq we sent (replay/fence resync), in which
  // case NextBatchSeq is realigned to CommittedSeq + 1 so future flushes aren't wedged behind a
  // gap the backend will keep rejecting. ALWAYS releases the flight slot. SentUpserts/SentDeletes
  // pair each sent id with the GENERATION current for it at send time.
  // FORK(hollowed-oath): SentCurrencyCount/SentGrantCount are the counts the batch took from the
  // front of the currency queues. bFinalRefusal is the ack's `final` flag: it freezes the component
  // (FreezeAfterFinalRefusal). bRebaseRequired is the ack's `rebaseRequired` flag: only that makes
  // the queue wait for a reload (see the currency staging block above). bNotApplied is the ack's
  // `notApplied` flag on a probe: the batch never committed, so it goes out again as a normal batch.
  // bNotSent is the sidecar's `notForwarded`: the batch never left the sidecar, so nothing about it
  // changes. An earlier attempt whose outcome is unknown stays unknown; otherwise the same rows and
  // entries go out again as a normal batch, with no probe.
  // A negative CommittedSeq means the reply carried no fence; a fence below SentSeq - 1 on an
  // error is a sequence gap, and the lane realigns to it.
  void CompleteItemFlush(
    const FString &Error,
    int64 CommittedSeq,
    const TArray<TPair<FString, uint64>> &SentUpserts,
    const TArray<TPair<FString, uint64>> &SentDeletes,
    int32 SentCurrencyCount = 0,
    int32 SentGrantCount = 0,
    bool bFinalRefusal = false,
    bool bRebaseRequired = false,
    bool bNotApplied = false,
    bool bNotSent = false
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

  // ONE monotonic source for every generation stamp in both maps above. Deliberately not per-id:
  // a per-id counter restarts whenever its entry is removed, which lets a stale in-flight ack
  // match a freshly recreated entry and clear a mutation that was never persisted.
  uint64 NextItemGeneration = 0;

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

  // A flush attempt deferred because a batch was already in flight. CompleteItemFlush runs it once
  // the slot frees. Exactly one is held: the detached barrier makes a single attempt, and chaining
  // an unbounded queue here would be a worse failure than the interim settle it replaces (#1534).
  TFunction<void()> PendingItemFlushRetry;

  // Currency ledger staging (see the public FORK block on QueueCurrency for the rules).
  TArray<FRedwoodCurrencyEntry> PendingCurrencyEntries;
  TArray<FString> PendingGrantKeys;

  // An unconfirmed batch: the counts at the front of the queues and the seq it went out under.
  bool bCurrencyBatchUnconfirmed = false;
  int32 UnconfirmedCurrencyCount = 0;
  int32 UnconfirmedGrantCount = 0;
  int64 UnconfirmedBatchSeq = 0;
  // Whether the in-flight claim re-sends the unconfirmed batch (see IsItemFlushReplayProbe).
  bool bInFlightIsCurrencyResend = false;

  bool bCurrencyRebaseRequired = false;
  FString WriterToken;

  // See IsFrozenAfterFinalRefusal.
  bool bFrozenAfterFinalRefusal = false;

  // See IsWriterFenceLost.
  bool bWriterFenceLost = false;

  // Re-chains every pending entry from Balance. Returns false, with nothing changed, when an entry
  // would overflow int64 or take the balance below zero.
  bool TryRechainPendingCurrency(int64 Balance);

  void ClearSentItemGenerations(
    const TArray<TPair<FString, uint64>> &SentUpserts,
    const TArray<TPair<FString, uint64>> &SentDeletes
  );
  void RemoveSentCurrency(int32 SentCurrencyCount, int32 SentGrantCount);
  // One Error line per staged entry and grant key, each starting with Lead ("<Lead> currency entry
  // ..."), so staff can rebuild what this server will never save.
  void LogStagedCurrency(const FString &Lead) const;
  void MarkCurrencyBatchUnconfirmed(int32 SentCurrencyCount, int32 SentGrantCount, int64 SentSeq);
  // FORK(hollowed-oath) END

public:
  /** Whether a flush attempt is already parked waiting for the in-flight batch's ack (#1534). */
  bool HasPendingItemFlushRetry() const { return static_cast<bool>(PendingItemFlushRetry); }
  /** Park a flush attempt to run when the in-flight batch completes. Overwrites nothing: callers
   *  check HasPendingItemFlushRetry first and settle immediately rather than displacing one. */
  void SetPendingItemFlushRetry(TFunction<void()> Retry) {
    PendingItemFlushRetry = MoveTemp(Retry);
  }
};
