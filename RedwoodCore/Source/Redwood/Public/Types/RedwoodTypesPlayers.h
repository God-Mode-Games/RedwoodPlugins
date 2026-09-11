// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"
#include "RedwoodTypesServers.h"

#include "RedwoodTypesPlayers.generated.h"

USTRUCT(BlueprintType)
struct FRedwoodPlayerOnlineStateRealm : public FRedwoodServerDetails {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString CharacterId;
};

UENUM(BlueprintType)
enum class ERedwoodFriendListType : uint8 {
  All,
  Active,
  PendingAll,
  PendingReceived,
  PendingSent,
  Blocked,
  Inactive,
  Unknown
};

USTRUCT(BlueprintType)
struct FRedwoodPlayer {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString PlayerId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Nickname;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  ERedwoodFriendListType FriendshipState = ERedwoodFriendListType::Unknown;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bOnline = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bPlaying = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodPlayerOnlineStateRealm OnlineStateRealm;
};

// FORK(hollowed-oath): delegate type carrying the player who asked to be a friend. Fork-added;
// upstream has no push for friend requests. Broadcast from the director listener in
// RedwoodClientInterface.cpp, relayed to the game by RedwoodClientGameSubsystem.
UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodFriendRequestReceivedDynamicDelegate, FRedwoodPlayer, Requester
);

USTRUCT(BlueprintType)
struct FRedwoodListPlayersOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodPlayer> Players;
};

typedef TDelegate<void(const FRedwoodListPlayersOutput &)>
  FRedwoodListPlayersOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodListPlayersOutputDynamicDelegate, FRedwoodListPlayersOutput, Data
);

USTRUCT(BlueprintType)
struct FRedwoodPlayerOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodPlayer Player;
};

typedef TDelegate<void(const FRedwoodPlayerOutput &)>
  FRedwoodPlayerOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodPlayerOutputDynamicDelegate, FRedwoodPlayerOutput, Data
);
