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

  // Per-container persistence channel (Task 8's Container table). Unlike the channels above,
  // this is not a single whole-blob field: ContainersVariableName names a
  // TArray<FRedwoodContainerRecord> UPROPERTY on the owning actor, and only the DIRTY records
  // (MarkContainersDirty) plus any pending deletions (MarkContainersDeleted) are sent to the
  // realm:characters:containers:upsert sidecar route on flush -- so a change to one bag doesn't
  // resend every other container. Load is a separate realm:characters:containers:load round trip
  // (see OnRedwoodContainersLoaded), because container rows are not part of
  // FRedwoodCharacterBackend / the realm:characters:set:server payload.
  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  bool bUseContainers = true;

  UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Redwood")
  FString ContainersVariableName = TEXT("Containers");

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

  // Records ContainerIds whose game-side record changed since the last flush. Dirty ids
  // accumulate (a second call before a flush unions in, it doesn't replace) so nothing is lost
  // if multiple mutations land in the same tick.
  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkContainersDirty(const TArray<FString> &DirtyContainerIds) {
    if (bUseContainers) {
      for (const FString &Id : DirtyContainerIds) {
        DirtyContainerIdSet.Add(Id);
      }
    }
  }

  // Records ContainerIds whose row should be DELETED on next flush (e.g. a bag that got
  // discarded). A deleted id is dropped from the dirty-upsert set too -- there's no point
  // upserting a row the same flush is about to delete.
  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkContainersDeleted(const TArray<FString> &DeletedContainerIds) {
    if (bUseContainers) {
      for (const FString &Id : DeletedContainerIds) {
        PendingDeletedContainerIds.AddUnique(Id);
        DirtyContainerIdSet.Remove(Id);
      }
    }
  }

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

  UFUNCTION(BlueprintPure, Category = "Redwood")
  bool IsContainersDirty() const {
    return DirtyContainerIdSet.Num() > 0 || PendingDeletedContainerIds.Num() > 0;
  }

  // Drained by URedwoodServerGameSubsystem::FlushContainersForCharacterComponent when building
  // the upsert payload; not BlueprintCallable (Blueprint has no TSet<FString> pin type).
  const TSet<FString> &GetDirtyContainerIds() const {
    return DirtyContainerIdSet;
  }

  const TArray<FString> &GetPendingDeletedContainerIds() const {
    return PendingDeletedContainerIds;
  }

  void ClearContainersDirtyState() {
    DirtyContainerIdSet.Empty();
    PendingDeletedContainerIds.Empty();
  }

  // Broadcast once the realm:characters:containers:load round trip completes and
  // ContainersVariableName has been populated on the owning actor. The load request is kicked
  // off at the END of RedwoodPlayerStateCharacterUpdated (after the fixed-channel
  // Deserialize*/OnRedwoodCharacterUpdated processing), so this ALWAYS fires strictly after
  // OnRedwoodCharacterUpdated for the same login/re-sync -- game code that needs both the
  // legacy dense inventory arrays AND Containers (e.g. AClient's load hook) should treat this as
  // a second, later-arriving signal, not assume Containers is already populated inside an
  // OnRedwoodCharacterUpdated handler.
  UPROPERTY(BlueprintAssignable, Category = "Redwood")
  FRedwoodDynamicDelegate OnRedwoodContainersLoaded;

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

  void ClearDirtyFlags() {
    bPlayerDataDirty = false;
    bCharacterCreatorDataDirty = false;
    bMetadataDirty = false;
    bEquippedInventoryDirty = false;
    bNonequippedInventoryDirty = false;
    bProgressDirty = false;
    bDataDirty = false;
    bAbilitySystemDirty = false;
    ClearContainersDirtyState();
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

  TSet<FString> DirtyContainerIdSet;
  TArray<FString> PendingDeletedContainerIds;
};
