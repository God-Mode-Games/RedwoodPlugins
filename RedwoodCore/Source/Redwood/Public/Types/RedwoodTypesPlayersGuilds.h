// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"
#include "RedwoodTypesGuilds.h"
#include "RedwoodTypesPlayers.h"

#include "RedwoodTypesPlayersGuilds.generated.h"

USTRUCT(BlueprintType)
struct FRedwoodPlayerData {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Nickname;

  // FORK(hollowed-oath) BEGIN: PlayerIdentity.role, carried so the game server can
  // resolve GM status (#1157). Stored as the raw backend integer rather than an
  // enum: the backend's PlayerRole comment states new tiers will be inserted
  // between the existing values, and >= comparison on the number stays correct
  // for a tier this build does not know about, where coercing to a known enum
  // member would misclassify it. Named tiers live in HollowedOath's
  // ERedwoodPlayerRole -- deliberately not here, since Redwood itself has no
  // role concept. Defaults to 0 (no privilege) so an unforked backend, or any
  // payload without the field, fails closed.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 Role = 0;
  // FORK(hollowed-oath) END

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bSelectedGuildValid = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodGuildInfo SelectedGuild;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Data = nullptr;
};