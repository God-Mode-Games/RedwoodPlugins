// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"

#include "RedwoodTypesCharacters.generated.h"

// One record of the per-container persistence channel (see URedwoodCharacterComponent's
// bUseContainers/ContainersVariableName/MarkContainersDirty). Unlike the fixed
// EquippedInventory/NonequippedInventory/etc. channels below (one whole-blob USTRUCT field,
// always resent in full when dirty), containers are transported as an ARRAY of these records so a
// flush can send only the ones that actually changed (plus a deletedContainerIds list) to the
// realm:characters:containers:upsert sidecar route. Contents is deliberately opaque here
// (an arbitrary JSON object) -- its shape is owned entirely by the game layer, matching how the
// USIOJsonObject* fields on FRedwoodCharacterBackend below work. Declared ABOVE
// FRedwoodCharacterBackend because that struct's Containers array needs the complete type.
USTRUCT(BlueprintType)
struct FRedwoodContainerRecord {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ContainerId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  uint8 Kind = 0;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Contents = nullptr;
};

USTRUCT(BlueprintType)
struct FRedwoodCharacterBackend {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime CreatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime UpdatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bArchived = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime ArchivedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString PlayerId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Name;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *CharacterCreatorData = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Metadata = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *EquippedInventory = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *NonequippedInventory = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Progress = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Data = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *AbilitySystem = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *RedwoodData = nullptr;

  // Container rows for this character, delivered in the SAME round trip as the rest of this
  // struct (the player-auth / character-load response) rather than a separate later-arriving
  // realm:characters:containers:load call -- this is what lets
  // URedwoodCharacterComponent::RedwoodPlayerStateCharacterUpdated() populate
  // ContainersVariableName BEFORE broadcasting OnRedwoodCharacterUpdated, so game code never sees
  // a character-updated event with containers still missing. Empty for offline/PIE-disk saves
  // (the on-disk character JSON has no "containers" field).
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodContainerRecord> Containers;
};

USTRUCT(BlueprintType)
struct FRedwoodListCharactersOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodCharacterBackend> Characters;
};

typedef TDelegate<void(const FRedwoodListCharactersOutput &)>
  FRedwoodListCharactersOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodListCharactersOutputDynamicDelegate,
  FRedwoodListCharactersOutput,
  Data
);

USTRUCT(BlueprintType)
struct FRedwoodGetCharacterOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodCharacterBackend Character;
};

typedef TDelegate<void(const FRedwoodGetCharacterOutput &)>
  FRedwoodGetCharacterOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodGetCharacterOutputDynamicDelegate, FRedwoodGetCharacterOutput, Data
);

USTRUCT(BlueprintType)
struct FRedwoodRealmContact {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString CharacterId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString CharacterName;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Description;
};

USTRUCT(BlueprintType)
struct FRedwoodListRealmContactsOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodRealmContact> Contacts;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodRealmContact> BlockedContacts;
};

typedef TDelegate<void(const FRedwoodListRealmContactsOutput &)>
  FRedwoodListRealmContactsOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodListRealmContactsOutputDynamicDelegate,
  FRedwoodListRealmContactsOutput,
  Data
);
