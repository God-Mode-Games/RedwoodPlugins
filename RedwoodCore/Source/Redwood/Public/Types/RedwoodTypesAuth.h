// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"

#include "RedwoodTypesAuth.generated.h"

UENUM(BlueprintType)
enum class ERedwoodAuthUpdateType : uint8 {
  Success,
  // FORK(hollowed-oath): never sent (HollowedOath#3607: an unverified login
  // is an Error with the server's words). The value stays because cooked
  // content names it (W_MainMenu, HollowedOath#1611); removing a UENUM value
  // orphans that pin.
  MustVerifyAccount,
  Error,
  Unknown
};

USTRUCT(BlueprintType)
struct FRedwoodAuthUpdate {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  ERedwoodAuthUpdateType Type = ERedwoodAuthUpdateType::Unknown;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Message;
};

typedef TDelegate<void(const FRedwoodAuthUpdate &)> FRedwoodAuthUpdateDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodAuthUpdateDynamicDelegate, FRedwoodAuthUpdate, Data
);
