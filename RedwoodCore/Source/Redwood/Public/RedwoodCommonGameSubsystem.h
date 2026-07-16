// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodModule.h"
#include "Types/RedwoodTypes.h"

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "RedwoodCommonGameSubsystem.generated.h"

class USIOJsonObject;
// FORK(hollowed-oath): forward decl for ResolveContainersRecordsArray's parameter (container helpers).
class URedwoodCharacterComponent;

UCLASS(BlueprintType)
class REDWOOD_API URedwoodCommonGameSubsystem : public UGameInstanceSubsystem {
  GENERATED_BODY()

public:
  // Begin USubsystem
  virtual void Initialize(FSubsystemCollectionBase &Collection) override;
  virtual void Deinitialize() override;
  // End USubsystem

  static void SaveCharacterJsonToDisk(TSharedPtr<FJsonObject> JsonObject);
  static void SaveCharacterToDisk(FRedwoodCharacterBackend &Character);

  static TArray<FRedwoodCharacterBackend> LoadAllCharactersFromDisk();

  static TSharedPtr<FJsonObject> LoadCharacterJsonFromDisk(FString CharacterId);
  static FRedwoodCharacterBackend LoadCharacterFromDisk(FString CharacterId);

  static uint8 GetCharactersOnDiskCount();

  static FRedwoodCharacterBackend ParseCharacter(
    TSharedPtr<FJsonObject> CharacterObj
  );
  static FRedwoodGameServerProxy ParseServerProxy(
    TSharedPtr<FJsonObject> ServerProxy
  );
  static FRedwoodGameServerInstance ParseServerInstance(
    TSharedPtr<FJsonObject> ServerInstance
  );
  static FRedwoodInitialLoadData ParseInitialLoadData(
    TSharedPtr<FJsonObject> InitialLoadData
  );

  static FRedwoodSyncItem ParseSyncItem(TSharedPtr<FJsonObject> SyncItem);
  static FRedwoodSyncItemState ParseSyncItemState(
    TSharedPtr<FJsonObject> SyncItemState
  );
  static FRedwoodSyncItemMovement ParseSyncItemMovement(
    TSharedPtr<FJsonObject> SyncItemMovement
  );
  static USIOJsonObject *ParseSyncItemData(TSharedPtr<FJsonObject> SyncItemData
  );

  static USIOJsonObject *SerializeBackendData(
    UObject *TargetObject, FString VariableName
  );

  static bool DeserializeBackendData(
    UObject *TargetObject,
    USIOJsonObject *SIOJsonObject,
    FString VariableName,
    int32 LatestSchemaVersion,
    bool &bErrored
  );

  UFUNCTION(BlueprintPure, Category = "Redwood")
  static bool ShouldUseBackend(UWorld *World);

  static ERedwoodFriendListType ParseFriendListType(FString StringValue);

  static ERedwoodGuildInviteType ParseGuildInviteType(FString StringValue);
  static FString SerializeGuildInviteType(ERedwoodGuildInviteType InviteType);

  static ERedwoodGuildAndAllianceMemberState ParseGuildAndAllianceMemberState(
    FString StringValue
  );
  static FString SerializeGuildAndAllianceMemberState(
    ERedwoodGuildAndAllianceMemberState State
  );

  static FRedwoodGuild ParseGuild(TSharedPtr<FJsonObject> GuildObject);
  static FRedwoodGuildInfo ParseGuildInfo(
    TSharedPtr<FJsonObject> GuildInfoObject
  );

  static FRedwoodAlliance ParseAlliance(TSharedPtr<FJsonObject> AllianceObj);

  static FRedwoodPlayerData ParsePlayerData(
    TSharedPtr<FJsonObject> PlayerDataObj
  );

  static FRedwoodPartyInvite ParsePartyInvite(
    const TSharedPtr<FJsonObject> &InviteObject
  );
  static TArray<FRedwoodPartyInvite> ParsePartyInvites(
    const TArray<TSharedPtr<FJsonValue>> &InvitesArray
  );

  static FRedwoodParty ParseParty(const TSharedPtr<FJsonObject> &PartyObj);

  // FORK(hollowed-oath) BEGIN: container wire-format helper declarations. Fork-added; definitions +
  // the full rationale live under a matching FORK marker in RedwoodCommonGameSubsystem.cpp. These
  // are the shared serialize/parse/resolve surface both persistence legs (backend sidecar upsert
  // and offline/PIE disk JSON) and the game module depend on -- preserve their signatures on merge.
  // Parses a wire "containers" JSON array (the {containerId, kind, contents} shape shared by the
  // player-auth response and the realm:characters:containers:load response) into
  // FRedwoodContainerRecord entries. Shared so both the character-arrival path
  // (RedwoodGameModeComponent's RunSidecarPlayerAuth) and any other container-bearing response
  // parse it identically.
  static TArray<FRedwoodContainerRecord> ParseContainerRecords(
    const TArray<TSharedPtr<FJsonValue>> &ContainersJsonArray
  );

  // Serializes one record into that same wire {containerId, kind, contents} shape -- the exact
  // inverse of ParseContainerRecords. Shared by the backend flush
  // (realm:characters:containers:upsert) and the offline/PIE disk save so the two legs cannot
  // drift: a character JSON written offline and a backend container row stay interchangeable.
  static TSharedPtr<FJsonObject> SerializeContainerRecord(
    const FRedwoodContainerRecord &Record
  );

  // Serializes a whole set of records into a wire "containers" array via
  // SerializeContainerRecord.
  static TArray<TSharedPtr<FJsonValue>> SerializeContainerRecords(
    const TArray<FRedwoodContainerRecord> &Records
  );

  // Resolves CharacterComponent->ContainersVariableName to a TArray<FRedwoodContainerRecord>
  // UPROPERTY on the component's data-holding object (the owning actor, or the component itself
  // when bStoreDataInActor is false -- same choice every other channel makes). Returns nullptr
  // (with an error logged) if the property is missing or isn't the expected array-of-struct
  // shape. Shared by the flush path (FlushContainersForCharacterComponent) and the arrival path
  // (RedwoodPlayerStateCharacterUpdated), which both need to reach the same array.
  static TArray<FRedwoodContainerRecord> *ResolveContainersRecordsArray(
    URedwoodCharacterComponent *CharacterComponent
  );
  // FORK(hollowed-oath) END

private:
};
