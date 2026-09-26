// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"

#include "RedwoodTypesRealms.generated.h"

USTRUCT(BlueprintType)
struct FRedwoodRealm {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime CreatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime UpdatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Name;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Uri;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bListed = false;

  // This is always an empty string from the ListRealms API call
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Secret;

  // FORK(hollowed-oath) BEGIN: the HollowedOath commit the realm's game server
  // was built from, so the client can offer only realms of its own build. Empty
  // when the director does not know it or is too old to send it.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Version;
  // FORK(hollowed-oath) END
};

USTRUCT(BlueprintType)
struct FRedwoodListRealmsOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodRealm> Realms;
};

typedef TDelegate<void(const FRedwoodListRealmsOutput &)>
  FRedwoodListRealmsOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodListRealmsOutputDynamicDelegate, FRedwoodListRealmsOutput, Output
);