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

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodGetCampLayoutOutputDynamicDelegate,
  FRedwoodGetCampLayoutOutput,
  Data
);
