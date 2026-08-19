// Copyright Incanta Games. All Rights Reserved.

#pragma once

#include "RedwoodTypesCommon.h"

#include "RedwoodTypesServers.generated.h"

USTRUCT(BlueprintType)
struct FRedwoodGameServerProxy {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime CreatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime UpdatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime EndedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bEnded = false;

  // Start common properties for GameServerInstance loading details

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Name;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ModeId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString MapId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bContinuousPlay = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Password;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ShortCode;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 MaxPlayersPerShard = 0;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Data = nullptr;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString OwnerPlayerId;

  // End common properties for GameServerInstance loading details

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Region;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bStartOnBoot = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FString> Zones;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 NumPlayersToAddShard = -1;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 NumMinutesToDestroyEmptyShard = -1;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bPublic = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bProxyEndsWhenCollectionEnds = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bCollectionEndsWhenAnyShardEnds = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bHasPassword = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  int32 CurrentPlayers = 0;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ActiveCollectionId;
};

USTRUCT(BlueprintType)
struct FRedwoodGameServerInstance {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Id;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime CreatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime UpdatedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ProviderId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime StartedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FDateTime EndedAt;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bEnded = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Connection;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Channel;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ZoneName;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ShardName;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ContainerId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString CollectionId;
};

USTRUCT(BlueprintType)
struct FRedwoodCreateProxyInput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Name;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Region;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ModeId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString MapId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bPublic = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bProxyEndsWhenCollectionEnds = true;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bContinuousPlay = false;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Password;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ShortCode;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  USIOJsonObject *Data = nullptr;

  // Setting this to true is only available for admins
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  bool bStartOnBoot = false;
};

USTRUCT(BlueprintType)
struct FRedwoodListProxiesOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FRedwoodGameServerProxy> Proxies;
};

typedef TDelegate<void(const FRedwoodListProxiesOutput &)>
  FRedwoodListProxiesOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodListProxiesOutputDynamicDelegate, FRedwoodListProxiesOutput, Data
);

USTRUCT(BlueprintType)
struct FRedwoodCreateProxyOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ProxyReference;
};

typedef TDelegate<void(const FRedwoodCreateProxyOutput &)>
  FRedwoodCreateProxyOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodCreateProxyOutputDynamicDelegate, FRedwoodCreateProxyOutput, Data
);

USTRUCT(BlueprintType)
struct FRedwoodJoinServerOutput {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Error;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ConnectionUri;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Token;
};

typedef TDelegate<void(const FRedwoodJoinServerOutput &)>
  FRedwoodJoinServerOutputDelegate;

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodJoinServerOutputDynamicDelegate, FRedwoodJoinServerOutput, Data
);

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodConnectToServerDynamicDelegate, FString, ConsoleCommand
);

UDELEGATE()
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
  FRedwoodStitchToServerDynamicDelegate, FURL, URL
);

USTRUCT(BlueprintType)
struct FRedwoodServerDetails {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString RealmName;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ProxyId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString InstanceId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ZoneName;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString ShardName;
};

// FORK(hollowed-oath) BEGIN: the answer to
// URedwoodServerGameSubsystem::RequestPlayerRoleChange, the game-server side of
// the in-game /gm command. Fork-added.
//
// bOk is true only when the backend reports no error, which means the role is
// committed. NewRole is the tier the backend committed, which is not always the
// tier that was asked for, and is 0 on every refusal. Error is the backend
// string, carried through WITHOUT translation: the plugin knows nothing about
// roles, so the game owns every word a player reads.
//
// Three plain parameters rather than the output struct the other requests use.
// The answer has no field the plugin must parse into a Redwood type, and a
// struct here would only make the game unpack it again.
typedef TDelegate<void(bool, int32, const FString &)>
  FRedwoodSetPlayerRoleOutputDelegate;
// FORK(hollowed-oath) END
