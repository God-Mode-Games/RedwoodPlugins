// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Types/RedwoodTypesAccountDocuments.h"

// The parts of the account document calls that do not need the subsystem: the
// checks, the reply parsing and the offline (no backend) store. They are here
// so that tests can call them directly. See
// URedwoodServerGameSubsystem::GetAccountDocument.
namespace RedwoodAccountDocuments {
  // ASCII letters, digits, '_' and '-', 1 to MaxIdLength characters, as the
  // backend checks the player id and the key.
  REDWOOD_API bool IsValidId(const FString &Id);

  // True when Data, as compact JSON, is bigger than MaxDataBytes.
  REDWOOD_API bool IsTooLarge(const TSharedRef<FJsonObject> &Data);

  // True when objects and arrays in Data nest deeper than MaxDepth, Data
  // being level 1. Check it before anything that serializes Data, because
  // the serializer recurses.
  REDWOOD_API bool IsTooDeep(const TSharedRef<FJsonObject> &Data);

  REDWOOD_API FRedwoodGetAccountDocumentOutput
  ParseGetReply(const TArray<TSharedPtr<FJsonValue>> &Response);

  REDWOOD_API FRedwoodPutAccountDocumentOutput
  ParsePutReply(const TArray<TSharedPtr<FJsonValue>> &Response);

  // The offline store: Saved/Persistence/AccountDocuments/<PlayerId>/<Key>.json.
  // The ids must be valid; the subsystem checks them first.
  REDWOOD_API FString GetOfflinePath(const FString &PlayerId, const FString &Key);

  REDWOOD_API FRedwoodGetAccountDocumentOutput
  ReadOffline(const FString &PlayerId, const FString &Key);

  REDWOOD_API FRedwoodPutAccountDocumentOutput WriteOffline(
    const FString &PlayerId,
    const FString &Key,
    const TSharedRef<FJsonObject> &Data,
    int32 ExpectedRevision
  );
} // namespace RedwoodAccountDocuments
