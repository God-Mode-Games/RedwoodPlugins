// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"

#include "RedwoodTypesCharacters.generated.h"

// FORK(hollowed-oath) BEGIN: FRedwoodItemRecord, the transport type for HollowedOath's per-item
// inventory. Entirely fork-added; no upstream counterpart. Replaces the earlier per-container
// shape (FRedwoodContainerRecord, RedwoodPlugins#17) with a flat per-item record -- each row is
// one item instance addressed by (Domain, Slot) instead of nesting inside an opaque
// per-container Contents blob. Backend counterpart lives on RedwoodBackend's
// feat/item-persistence line. Must stay declared ABOVE FRedwoodCharacterBackend (its Items array
// needs the complete type).
// One record of the per-item persistence channel (see URedwoodCharacterComponent's
// bUseItems/ItemsVariableName/MarkItemsDirty). Unlike the fixed
// EquippedInventory/NonequippedInventory/etc. channels below (one whole-blob USTRUCT field,
// always resent in full when dirty), items are transported as an ARRAY of these records so a
// flush can send only the ones that actually changed (plus a deletes list) to the
// realm:characters:items:flush sidecar route. ParentId establishes nesting (empty string = root
// item, matching the wire's null) -- sockets and other contained items point at their owner via
// this field instead of living inside an opaque container blob. Attributes is deliberately opaque
// here (an arbitrary JSON object) -- its shape is owned entirely by the game layer, matching how
// the USIOJsonObject* fields on FRedwoodCharacterBackend below work.
USTRUCT(BlueprintType)
struct FRedwoodItemRecord {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ParentId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Domain;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Slot = 0;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Quantity = 1;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString TemplateId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Attributes = nullptr;
};
// FORK(hollowed-oath) END

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

  // FORK(hollowed-oath) BEGIN: fork-added Items field on the otherwise-upstream character
  // struct, replacing the earlier per-container Containers field (FRedwoodContainerRecord,
  // RedwoodPlugins#17). This is the load-leg carrier -- rows ride the character's own round trip,
  // letting the component populate the game array before OnRedwoodCharacterUpdated. Preserve on
  // merge.
  // Item rows for this character, delivered in the SAME round trip as the rest of this struct
  // (the player-auth / character-load response) rather than a separate later-arriving
  // realm:characters:items:load call -- this is what lets
  // URedwoodCharacterComponent::RedwoodPlayerStateCharacterUpdated() populate the game-side
  // inventory BEFORE broadcasting OnRedwoodCharacterUpdated, so game code never sees a
  // character-updated event with items still missing. Offline/PIE-disk saves populate this the
  // same way, from the "items" array the on-disk character JSON carries inline (there is no Item
  // table to read there) -- see URedwoodCommonGameSubsystem::SaveCharacterToDisk and
  // URedwoodServerGameSubsystem::AppendOfflineItemRows. The backend's character-LIST response also
  // carries this array inline on each character object (equipment/cosmetic/socket rows) for the
  // character-select preview; ParseCharacter reads it off the object the same way the offline leg
  // does. InventorySeq is fork-added bookkeeping: a per-character mutation sequence counter used to
  // detect stale/racing flushes (the live flush fence). Row-per-item is now the NATIVE persistence
  // model, so the former one-time blob->row migration marker (InventoryRowsMigrated) is deleted --
  // there is nothing to migrate from. Backend counterpart for both remaining fields lives on
  // RedwoodBackend's feat/item-persistence line.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodItemRecord> Items;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int64 InventorySeq = 0;
  // FORK(hollowed-oath) END
};

// FORK(hollowed-oath) BEGIN: FRedwoodTradeRootPlacement, forward-declared here alongside the item
// wire types it composes with. Entirely fork-added; no upstream counterpart. Identifies where a
// root item (ParentId empty on FRedwoodItemRecord) sits in a trade offer -- trade-window code
// added in a later task (RedwoodPlugins plan Task 4) resolves an FRedwoodItemRecord.Id to its
// placement via (Domain, Slot) without needing the full item payload. Declared now, unused until
// that task lands, to keep this header's item-model diff a single commit.
//
// ParentId widens this beyond pure root placements: production trades move items between bags,
// i.e. content-child placements (an item socketed/nested inside a receiving-side bag item), not
// just top-level slots. Zero-value FString (empty) still means "root placement" -- the struct is
// shipped wire surface, so the empty-default reading is preserved rather than renaming the type
// or adding a separate root/child struct pair. Domain is correspondingly no longer limited to
// top-level inventory domains ("equipped", "nonequipped"); it also carries "content" for
// container-item placements, matching FRedwoodItemRecord.Domain's existing "content" value.
USTRUCT(BlueprintType)
struct FRedwoodTradeRootPlacement {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Domain;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Slot = 0;

  // Empty = root placement (lands directly in the receiving character's top-level inventory).
  // Non-empty = the receiver-side bag item's InstanceId this item is placed inside, for
  // content-child (nested/socketed) placements. Mirrors FRedwoodItemRecord.ParentId's semantics;
  // wire serialization mirrors its null convention too (see SerializeItemRecord / ParseItemRecords
  // in RedwoodCommonGameSubsystem.cpp) -- empty here means JSON null on the wire, not "".
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ParentId;
};
// FORK(hollowed-oath) END

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
