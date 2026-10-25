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

// FORK(hollowed-oath) BEGIN: account-wide chat mutes. Upstream has no mute;
// the record lives on the director in the RedwoodBackend fork. One mute covers
// every character and every channel of the account, so it is keyed by player.
// Plain structs and plain delegates, like the role change: no Blueprint
// reaches them, and the game owns every word a player reads.
//
// Until is Unix milliseconds the mute ends at, and 0 means no end. The answer
// to a set carries the end the director committed, which it computed from the
// duration, so the game shows the same moment Bifrost refuses with.
struct FRedwoodSetMuteOutput {
  FString Error;
  int64 Until = 0;
};

typedef TDelegate<void(const FRedwoodSetMuteOutput &)>
  FRedwoodSetMuteOutputDelegate;

// What the realm holds for one player: the game server's copy. bMuted tells a
// player with no mute from one muted with no end, which Until alone cannot
// (both are 0). On a refusal bMuted is false and Error says why.
struct FRedwoodPlayerMuteOutput {
  FString Error;
  bool bMuted = false;
  int64 Until = 0;
  FString Reason;
};

typedef TDelegate<void(const FRedwoodPlayerMuteOutput &)>
  FRedwoodPlayerMuteOutputDelegate;

// The realm's push after a set or a clear, to the game server that hosts a
// character of the player. After a clear: bMuted false, Until 0, Reason "".
struct FRedwoodPlayerMuteChanged {
  FString PlayerId;
  bool bMuted = false;
  int64 Until = 0;
  FString Reason;
};

DECLARE_MULTICAST_DELEGATE_OneParam(
  FRedwoodPlayerMuteChangedDelegate, const FRedwoodPlayerMuteChanged &
);
// FORK(hollowed-oath) END
