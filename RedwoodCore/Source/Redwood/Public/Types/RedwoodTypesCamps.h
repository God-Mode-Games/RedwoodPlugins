// Copyright Incanta Games. All Rights Reserved.

// FORK(hollowed-oath): account-owned camp record, stored by the realm
// `camps` table. See Documentation/Plans/PlayerCamp.md in HollowedOath.

#pragma once

#include "RedwoodTypesCommon.h"

#include "SIOJsonObject.h"

#include "RedwoodTypesCamps.generated.h"

USTRUCT(BlueprintType)
struct FRedwoodCampLayout {
  GENERATED_BODY()

  // False when the account has no camp record yet. The other fields are then
  // unset.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bFound = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 SchemaVersion = 0;

  // The row's revision. A put must name the revision it was based on.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Revision = 0;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Record = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Layout = nullptr;
};

USTRUCT(BlueprintType)
struct FRedwoodGetCampLayoutOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodCampLayout Camp;
};

typedef TDelegate<void(const FRedwoodGetCampLayoutOutput &)>
  FRedwoodGetCampLayoutOutputDelegate;

USTRUCT(BlueprintType)
struct FRedwoodPutCampLayoutOutput {
  GENERATED_BODY()

  // Set when the put could not be tried or failed. A conflict is not an error.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  // True when the row was not at the expected revision, so nothing was written.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bConflict = false;

  // The new revision after a successful put.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Revision = 0;

  // True when the backend refused the camp for its size. Retrying the same
  // camp cannot succeed, so callers branch on this and not on Error's text.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bTooLarge = false;
};

typedef TDelegate<void(const FRedwoodPutCampLayoutOutput &)>
  FRedwoodPutCampLayoutOutputDelegate;
