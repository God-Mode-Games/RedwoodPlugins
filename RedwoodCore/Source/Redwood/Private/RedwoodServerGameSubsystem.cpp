// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodServerGameSubsystem.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodClientExecCommand.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodGameModeAsset.h"
#include "RedwoodGameplayTags.h"
#include "RedwoodMapAsset.h"
#include "RedwoodPersistenceComponentInterface.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodSettings.h"
#include "RedwoodSyncComponent.h"
#include "RedwoodSyncItemAsset.h"
// FORK(hollowed-oath): FRedwoodItemRecord + FRedwoodTradeRootPlacement live here; added for the
// item flush / migrate / trade / offline helpers below.
#include "Types/RedwoodTypesCharacters.h"
#include "Types/RedwoodTypesSync.h"

#if WITH_EDITOR
  #include "RedwoodEditorSettings.h"
#endif

#include "Engine/AssetManager.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetStringLibrary.h"

#include "SIOJsonValue.h"
#include "SocketIOClient.h"

void URedwoodServerGameSubsystem::Initialize(
  FSubsystemCollectionBase &Collection
) {
  Super::Initialize(Collection);

  UWorld *World = GetWorld();

  if (IsValid(World) &&
      (World->GetNetMode() == ENetMode::NM_DedicatedServer ||
       World->GetNetMode() == ENetMode::NM_ListenServer)) {
    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Initializing RedwoodServerGameSubsystem for server")
    );

    FPrimaryAssetType GameModeAssetType =
      URedwoodGameModeAsset::StaticClass()->GetFName();
    FPrimaryAssetType MapAssetType =
      URedwoodMapAsset::StaticClass()->GetFName();

    UAssetManager &AssetManager = UAssetManager::Get();

    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Waiting for RedwoodGameModeAsset amd RedwoodMapAsset to load")
    );

    // Load Redwood GameMode and Map assets so we can know which underlying GameMode and Map to load later
    TSharedPtr<FStreamableHandle> HandleModes =
      AssetManager.LoadPrimaryAssetsWithType(GameModeAssetType);
    TSharedPtr<FStreamableHandle> HandleMaps =
      AssetManager.LoadPrimaryAssetsWithType(MapAssetType);

    if (!HandleModes.IsValid() || !HandleMaps.IsValid()) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "Failed to load RedwoodGameModeAsset or RedwoodMapAsset asset types; not initializing RedwoodServerGameSubsystem"
        )
      );
      return;
    }

    HandleModes->WaitUntilComplete();
    HandleMaps->WaitUntilComplete();

    TArray<UObject *> GameModesAssets;
    TArray<UObject *> MapsAssets;

    AssetManager.GetPrimaryAssetObjectList(GameModeAssetType, GameModesAssets);
    AssetManager.GetPrimaryAssetObjectList(MapAssetType, MapsAssets);

    for (UObject *Object : GameModesAssets) {
      URedwoodGameModeAsset *RedwoodGameMode =
        Cast<URedwoodGameModeAsset>(Object);
      if (ensure(RedwoodGameMode)) {
        if (!RedwoodGameMode->RedwoodId.IsEmpty()) {
          if (RedwoodGameMode->GameModeType == ERedwoodGameModeType::GameModeBase) {
            GameModeClasses.Add(
              RedwoodGameMode->RedwoodId, RedwoodGameMode->GameModeBaseClass
            );
          } else {
            GameModeClasses.Add(
              RedwoodGameMode->RedwoodId, RedwoodGameMode->GameModeClass
            );
          }
        }
      }
    }

    for (UObject *Object : MapsAssets) {
      URedwoodMapAsset *RedwoodMap = Cast<URedwoodMapAsset>(Object);
      if (ensure(RedwoodMap)) {
        if (!RedwoodMap->RedwoodId.IsEmpty()) {
          Maps.Add(RedwoodMap->RedwoodId, RedwoodMap->MapId);
        }
      }
    }

    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Loaded %d GameMode assets and %d Map assets"),
      GameModeClasses.Num(),
      Maps.Num()
    );

    FPrimaryAssetType SyncItemAssetType =
      URedwoodSyncItemAsset::StaticClass()->GetFName();
    TSharedPtr<FStreamableHandle> HandleSyncItems =
      AssetManager.LoadPrimaryAssetsWithType(SyncItemAssetType);

    if (HandleSyncItems.IsValid()) {
      UE_LOG(LogRedwood, Log, TEXT("Waiting for RedwoodSyncItemAsset to load"));

      HandleSyncItems->WaitUntilComplete();
      TArray<UObject *> SyncItemAssets;
      AssetManager.GetPrimaryAssetObjectList(SyncItemAssetType, SyncItemAssets);

      for (UObject *Object : SyncItemAssets) {
        URedwoodSyncItemAsset *RedwoodSyncItem =
          Cast<URedwoodSyncItemAsset>(Object);
        if (ensure(RedwoodSyncItem)) {
          if (!RedwoodSyncItem->RedwoodTypeId.IsEmpty()) {
            SyncItemTypesByTypeId.Add(
              RedwoodSyncItem->RedwoodTypeId, RedwoodSyncItem
            );
            SyncItemTypesByPrimaryAssetId.Add(
              RedwoodSyncItem->GetPrimaryAssetId().ToString(), RedwoodSyncItem
            );
          }
        }
      }

      UE_LOG(
        LogRedwood, Log, TEXT("Loaded %d SyncItem assets"), SyncItemAssets.Num()
      );
    } else {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT(
          "No RedwoodSyncItemAsset asset type in the Asset Manager; continuing without loading"
        )
      );
    }

    URedwoodSettings *RedwoodSettings = GetMutableDefault<URedwoodSettings>();
    if (RedwoodSettings->bServersAutoConnectToSidecar) {
      if (URedwoodCommonGameSubsystem::ShouldUseBackend(World)) {
        InitializeSidecar();
      }
    }

    UGameplayMessageSubsystem &MessageSubsystem =
      UGameplayMessageSubsystem::Get(this);
    ListenerHandle = MessageSubsystem.RegisterListener(
      TAG_Redwood_Shutdown_Instance,
      this,
      &URedwoodServerGameSubsystem::OnShutdownMessage
    );

    UE_LOG(
      LogRedwood, Log, TEXT("Finished initializing RedwoodServerGameSubsystem")
    );
  }
}

void URedwoodServerGameSubsystem::OnShutdownMessage(
  FGameplayTag InChannel, const FRedwoodReason &Message
) {
  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Received shutdown message, reason: %s"),
    *Message.Reason
  );
  bIsShuttingDown = true;
}

void URedwoodServerGameSubsystem::Deinitialize() {
  Super::Deinitialize();

  if (TimerHandle_UpdateSidecar.IsValid()) {
    GetGameInstance()->GetTimerManager().ClearTimer(TimerHandle_UpdateSidecar);
  }

  if (TimerHandle_UpdateSidecarLoading.IsValid()) {
    GetGameInstance()->GetTimerManager().ClearTimer(
      TimerHandle_UpdateSidecarLoading
    );
  }

  if (Sidecar.IsValid()) {
    ISocketIOClientModule::Get().ReleaseNativePointer(Sidecar);
    Sidecar = nullptr;
  }
}

void URedwoodServerGameSubsystem::InitializeSidecar() {
  Sidecar = ISocketIOClientModule::Get().NewValidNativePointer();

  Sidecar->OnEvent(
    TEXT("realm:servers:session:load-map"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      UE_LOG(LogRedwood, Log, TEXT("Received message to load a map"));
      const TSharedPtr<FJsonObject> *Object;

      TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);

      if (Message->TryGetObject(Object) && Object) {
        UE_LOG(LogRedwood, Log, TEXT("LoadMap message is valid object"));
        TSharedPtr<FJsonObject> ActualObject = *Object;
        RequestId = ActualObject->GetStringField(TEXT("requestId"));
        RealmName = ActualObject->GetStringField(TEXT("realmName"));
        ProxyId = ActualObject->GetStringField(TEXT("proxyId"));
        InstanceId = ActualObject->GetStringField(TEXT("instanceId"));
        Name = ActualObject->GetStringField(TEXT("name"));
        MapId = ActualObject->GetStringField(TEXT("mapId"));
        ModeId = ActualObject->GetStringField(TEXT("modeId"));
        bContinuousPlay = ActualObject->GetBoolField(TEXT("continuousPlay"));
        ActualObject->TryGetStringField(TEXT("password"), Password);
        ActualObject->TryGetStringField(TEXT("shortCode"), ShortCode);
        MaxPlayers = ActualObject->GetIntegerField(TEXT("maxPlayers"));
        ActualObject->TryGetStringField(TEXT("ownerPlayerId"), OwnerPlayerId);
        Channel = ActualObject->GetStringField(TEXT("channel"));
        ActualObject->TryGetStringField(TEXT("parentProxyId"), ParentProxyId);

        UE_LOG(
          LogRedwood,
          Log,
          TEXT("LoadMap message has map (%s), mode (%s), and channel (%s)"),
          *MapId,
          *ModeId,
          *Channel
        );

        if (Channel.Contains(":")) {
          TArray<FString> ChannelParts;
          Channel.ParseIntoArray(ChannelParts, TEXT(":"), true);

          if (ChannelParts.Num() > 0) {
            ZoneName = ChannelParts[0];
          }

          if (ChannelParts.Num() > 1) {
            ShardName = ChannelParts[1];
          }
        }

        TSubclassOf<AGameModeBase> *GameModeToLoad =
          GameModeClasses.Find(ModeId);
        FPrimaryAssetId *MapToLoad = Maps.Find(MapId);

        if (GameModeToLoad == nullptr || MapToLoad == nullptr) {
          UE_LOG(
            LogRedwood, Error, TEXT("Failed to find GameMode or Map to load")
          );
          Response->SetStringField(
            TEXT("error"), TEXT("Invalid ModeId or MapId")
          );
          Sidecar->Emit(
            TEXT("realm:servers:session:load-map:response"), Response
          );
          return;
        }

        FString Error;
        FURL Url;
        Url.Protocol = "unreal";
        Url.Map = MapToLoad->PrimaryAssetName.ToString();

        // The preferred method to retrieve these variables is via this subsystem public
        // member variables, but we add them to the URL for convenience if needed
        Url.AddOption(*FString("requestId=" + RequestId));
        Url.AddOption(*FString("name=" + Name));
        Url.AddOption(*FString("mapId=" + MapId));
        Url.AddOption(*FString("modeId=" + ModeId));
        Url.AddOption(
          *FString("continuousPlay=" + FString::FromInt(bContinuousPlay))
        );
        Url.AddOption(*FString("password=" + Password));
        Url.AddOption(*FString("shortCode=" + ShortCode));
        Url.AddOption(*FString("maxPlayers=" + FString::FromInt(MaxPlayers)));
        Url.AddOption(*FString("ownerPlayerId=" + OwnerPlayerId));
        Url.AddOption(*FString("game=" + (*GameModeToLoad)->GetPathName()));

        const TSharedPtr<FJsonObject> *DataObj;
        if (ActualObject->TryGetObjectField(TEXT("data"), DataObj)) {
          // Iterate the JSON object directly; deref of the key works whether it
          // is stored as FString or UE::FSharedString (5.8 FStringView keys).
          for (const auto &Pair : (*DataObj)->Values) {
            const FString Key = *Pair.Key;
            FString Value;
            if ((*DataObj)->TryGetStringField(Key, Value)) {
              Url.AddOption(*FString(Key + "=" + Value));
            }
          }
        }

        FString Command = FString::Printf(TEXT("open %s"), *Url.ToString());
        GetGameInstance()->GetEngine()->DeferredCommands.Add(Command);

        Response->SetStringField(TEXT("error"), TEXT(""));
      } else {
        UE_LOG(LogRedwood, Error, TEXT("LoadMap message is not valid object"));
        Response->SetStringField(TEXT("error"), TEXT("Invalid request"));
      }

      Sidecar->Emit(TEXT("realm:servers:session:load-map:response"), Response);
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:parties:update:bulk"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        const TSharedPtr<FJsonObject> *PartiesObj;
        if (!(*Object)->TryGetObjectField(TEXT("parties"), PartiesObj)) {
          UE_LOG(
            LogRedwood,
            Error,
            TEXT("Party bulk update message has no parties object")
          );
          return;
        }

        TrackedParties.Empty();

        for (const auto &Pair : (*PartiesObj)->Values) {
          const TSharedPtr<FJsonObject> *PartyObj;
          if (Pair.Value->TryGetObject(PartyObj) && PartyObj) {
            // Deref the JSON key (*Pair.Key) to FString; works whether keys are
            // stored as FString or UE::FSharedString (5.8 FStringView keys).
            TrackedParties.Add(
              FString(*Pair.Key), URedwoodCommonGameSubsystem::ParseParty(*PartyObj)
            );
          }
        }

        UpdatePlayerStateComponentPartyIds();

        OnTrackedPartiesUpdated.Broadcast();
      }
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:parties:update:single"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (!Message->TryGetObject(Object) || !Object) {
        return;
      }

      FString PartyId;
      if (!(*Object)->TryGetStringField(TEXT("partyId"), PartyId)) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Party single update message has no partyId")
        );
        return;
      }

      // `party` is null when the party disbanded or this server no
      // longer hosts any of its members; otherwise we track it only if
      // we host a current member (the backend also sends single updates
      // to the servers of members who just left, so they can drop it).
      const TSharedPtr<FJsonObject> *PartyObj;
      FRedwoodParty Party;
      bool bHasParty = false;
      if ((*Object)->TryGetObjectField(TEXT("party"), PartyObj) && PartyObj) {
        Party = URedwoodCommonGameSubsystem::ParseParty(*PartyObj);
        bHasParty = Party.bValid;
      }

      if (bHasParty && DoesServerHostPartyMember(Party)) {
        TrackedParties.Add(PartyId, Party);
      } else {
        TrackedParties.Remove(PartyId);
      }

      UpdatePlayerStateComponentPartyIds();

      OnTrackedPartiesUpdated.Broadcast();
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:servers:session:sync:new"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        TSharedPtr<FJsonObject> ActualObject = *Object;
        FRedwoodSyncItem SyncItem =
          URedwoodCommonGameSubsystem::ParseSyncItem(ActualObject);
        UpdateSyncItem(SyncItem);
      }
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:servers:session:sync:state"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        TSharedPtr<FJsonObject> ActualObject = *Object;
        FString ItemId = ActualObject->GetStringField(TEXT("id"));

        bool bCleanupEntry = true;
        if (TWeakObjectPtr<URedwoodSyncComponent>* WeakPtr = SyncItemComponentsById.Find(ItemId))
        {
          if (URedwoodSyncComponent *SyncItemComponent = WeakPtr->Get()) {
            bCleanupEntry = false;

            FRedwoodSyncItemState SyncItemState =
              URedwoodCommonGameSubsystem::ParseSyncItemState(ActualObject);

            UpdateSyncItemState(SyncItemComponent, SyncItemState);
          }
        }

        if (bCleanupEntry) {
          // Clean up dead entry
          SyncItemComponentsById.Remove(ItemId);
        }
      }
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:servers:session:sync:movement"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        TSharedPtr<FJsonObject> ActualObject = *Object;
        FString ItemId = ActualObject->GetStringField(TEXT("id"));

        bool bCleanupEntry = true;
        if (TWeakObjectPtr<URedwoodSyncComponent>* WeakPtr = SyncItemComponentsById.Find(ItemId))
        {
          if (URedwoodSyncComponent *SyncItemComponent = WeakPtr->Get()) {
            bCleanupEntry = false;

            TSharedPtr<FJsonObject> MovementObj =
              ActualObject->GetObjectField(TEXT("movement"));
            FRedwoodSyncItemMovement SyncItemMovement =
              URedwoodCommonGameSubsystem::ParseSyncItemMovement(MovementObj);

            UpdateSyncItemMovement(SyncItemComponent, SyncItemMovement);
          }
        }

        if (bCleanupEntry) {
          // Clean up dead entry
          SyncItemComponentsById.Remove(ItemId);
        }
      }
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:servers:session:sync:data"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        TSharedPtr<FJsonObject> ActualObject = *Object;
        FString ItemId = ActualObject->GetStringField(TEXT("id"));

        bool bCleanupEntry = true;
        if (TWeakObjectPtr<URedwoodSyncComponent>* WeakPtr = SyncItemComponentsById.Find(ItemId))
        {
          if (URedwoodSyncComponent *SyncItemComponent = WeakPtr->Get()) {
            bCleanupEntry = false;

            TSharedPtr<FJsonObject> DataObj =
              ActualObject->GetObjectField(TEXT("data"));
            USIOJsonObject *SyncItemData =
              URedwoodCommonGameSubsystem::ParseSyncItemData(DataObj);

            UpdateSyncItemData(SyncItemComponent, SyncItemData);
          }
        }

        if (bCleanupEntry) {
          // Clean up dead entry
          SyncItemComponentsById.Remove(ItemId);
        }
      }
    }
  );

  Sidecar->OnEvent(
    TEXT("realm:players:data-changed"),
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        TSharedPtr<FJsonObject> ActualObject = *Object;
        FString PlayerId = ActualObject->GetStringField(TEXT("playerId"));

        // find the player state with this PlayerId
        UWorld *World = GetWorld();
        if (IsValid(World)) {
          for (APlayerState *PlayerState : World->GetGameState()->PlayerArray) {
            URedwoodPlayerStateComponent *PlayerStateComponent =
              PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();
            if (!IsValid(PlayerStateComponent)) {
              continue;
            }

            if (PlayerStateComponent->RedwoodPlayer.Id == PlayerId) {
              // Found the player state
              TSharedPtr<FJsonObject> PlayerData =
                ActualObject->GetObjectField(TEXT("data"));
              PlayerStateComponent->SetRedwoodPlayer(
                URedwoodCommonGameSubsystem::ParsePlayerData(PlayerData)
              );
              break;
            }
          }
        }
      }
    }
  );

  Sidecar->OnConnectedCallback =
    [this](const FString &InSocketId, const FString &InSessionId) {
      if (Sidecar.IsValid()) {
        GetGameInstance()->GetTimerManager().SetTimer(
          TimerHandle_UpdateSidecarLoading,
          this,
          &URedwoodServerGameSubsystem::SendUpdateToSidecar,
          UpdateSidecarLoadingRate,
          true // loop
        );

        GetGameInstance()->GetTimerManager().SetTimer(
          TimerHandle_UpdateSidecar,
          this,
          &URedwoodServerGameSubsystem::SendUpdateToSidecar,
          UpdateSidecarRate,
          true, // loop
          0.f // immediately trigger first one
        );
      }
    };

  FString SidecarPort = TEXT("3020"); // default port
  FParse::Value(FCommandLine::Get(), TEXT("SidecarPort="), SidecarPort);

  SidecarUri = TEXT("ws://127.0.0.1:") + SidecarPort;
  UE_LOG(LogRedwood, Log, TEXT("Connecting to Sidecar at %s"), *SidecarUri);

  // Sidecar will always be on the same host
  Sidecar->Connect(SidecarUri);
}

void URedwoodServerGameSubsystem::SendUpdateToSidecar() {
  if (Sidecar.IsValid()) {
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

    UWorld *World = GetWorld();

    if (IsValid(World)) {
      AGameModeBase *GameMode = World->GetAuthGameMode();
      if (IsValid(GameMode)) {
        bool bWorldStarted = World->GetRealTimeSeconds() > 0;
        if (bWorldStarted) {
          if (TimerHandle_UpdateSidecarLoading.IsValid()) {
            GetGameInstance()->GetTimerManager().ClearTimer(
              TimerHandle_UpdateSidecarLoading
            );
          }

          if (bIsShuttingDown) {
            JsonObject->SetStringField(TEXT("state"), TEXT("Stopping"));
          } else {
            ARedwoodGameModeBase *RedwoodGameModeBase =
              Cast<ARedwoodGameModeBase>(GameMode);

            if (RedwoodGameModeBase) {
              // We're not inheriting from AGameMode so we're not match based (theoretically)
              // so we just assume we're in a started state
              JsonObject->SetStringField(TEXT("state"), TEXT("Started"));
            } else {
              if (GameMode->HasMatchStarted()) {
                if (GameMode->HasMatchEnded()) {
                  JsonObject->SetStringField(TEXT("state"), TEXT("Ended"));
                } else {
                  JsonObject->SetStringField(TEXT("state"), TEXT("Started"));
                }
              } else {
                JsonObject->SetStringField(
                  TEXT("state"), TEXT("WaitingForPlayers")
                );
              }
            }
          }
        }
      } else {
        JsonObject->SetStringField(TEXT("state"), TEXT("LoadingMap"));
      }

      JsonObject->SetNumberField(TEXT("numPlayers"), GameMode->GetNumPlayers());
    } else {
      bool bStarted = World->GetRealTimeSeconds() > 0;
      JsonObject->SetStringField(TEXT("state"), TEXT("LoadingMap"));

      JsonObject->SetNumberField(TEXT("numPlayers"), 0);
    }

    // We get these options from the URL instead of the member variables to
    // ensure the map got loaded

#if WITH_EDITOR
    if (World->WorldType == EWorldType::PIE) {
      // we skip loading the level in PIE, so we just use the options
      // that got set earlier and assuming we're already in the correct
      // level
      JsonObject->SetStringField(TEXT("id"), RequestId);
      JsonObject->SetStringField(TEXT("mapId"), MapId);
      JsonObject->SetStringField(TEXT("modeId"), ModeId);
    } else
#endif
    {
      JsonObject->SetStringField(
        TEXT("id"), World->URL.GetOption(TEXT("requestId="), TEXT(""))
      );

      JsonObject->SetStringField(
        TEXT("mapId"), World->URL.GetOption(TEXT("mapId="), TEXT(""))
      );

      JsonObject->SetStringField(
        TEXT("modeId"), World->URL.GetOption(TEXT("modeId="), TEXT(""))
      );
    }

    Sidecar->Emit(TEXT("realm:servers:update-instance-state"), JsonObject);
  }
}

void URedwoodServerGameSubsystem::CallExecCommandOnAllClients(
  const FString &Command
) {
  // spawn ARedwoodClientExecCommand
  UWorld *World = GetWorld();
  if (IsValid(World)) {
    // spawn actor
    ARedwoodClientExecCommand *ExecCommand = Cast<ARedwoodClientExecCommand>(
      UGameplayStatics::BeginDeferredActorSpawnFromClass(
        this, ARedwoodClientExecCommand::StaticClass(), FTransform()
      )
    );
    if (ExecCommand) {
      ExecCommand->Command = Command;
      UGameplayStatics::FinishSpawningActor(ExecCommand, FTransform());
    }
  }
}

void URedwoodServerGameSubsystem::TravelPlayerToZoneTransform(
  APlayerController *PlayerController,
  const FString &InZoneName,
  const FTransform &InTransform,
  const FString &OptionalProxyId,
  bool bShouldStitch
) {
  FString UniqueId = PlayerController->PlayerState->GetUniqueId().ToString();

  URedwoodPlayerStateComponent *PlayerStateComponent =
    PlayerController->PlayerState
      ->FindComponentByClass<URedwoodPlayerStateComponent>();

  if (PlayerStateComponent) {
    PlayerStateComponent->InitTransferring();
  }

  FString PlayerId = UniqueId.Left(UniqueId.Find(TEXT(":")));
  FString CharacterId = UniqueId.RightChop(UniqueId.Find(TEXT(":")) + 1);

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  TArray<TSharedPtr<FJsonValue>> PlayersArray;
  TSharedPtr<FJsonObject> PlayerObject = MakeShareable(new FJsonObject);

  PlayerObject->SetStringField(TEXT("characterId"), CharacterId);

  TSharedPtr<FJsonObject> TransformOffset = MakeShareable(new FJsonObject);
  TSharedPtr<FJsonObject> VectorOffset = MakeShareable(new FJsonObject);
  VectorOffset->SetNumberField(TEXT("x"), 0);
  VectorOffset->SetNumberField(TEXT("y"), 0);
  VectorOffset->SetNumberField(TEXT("z"), 0);
  TransformOffset->SetObjectField(TEXT("location"), VectorOffset);
  TransformOffset->SetObjectField(TEXT("rotation"), VectorOffset);
  TSharedPtr<FJsonObject> ControlRotation = MakeShareable(new FJsonObject);
  ControlRotation->SetNumberField(
    TEXT("x"), PlayerController->GetControlRotation().Roll
  );
  ControlRotation->SetNumberField(
    TEXT("y"), PlayerController->GetControlRotation().Pitch
  );
  ControlRotation->SetNumberField(
    TEXT("z"), PlayerController->GetControlRotation().Yaw
  );
  TransformOffset->SetObjectField(TEXT("controlRotation"), ControlRotation);
  PlayerObject->SetObjectField(TEXT("transformOffset"), TransformOffset);

  PlayersArray.Add(MakeShareable(new FJsonValueObject(PlayerObject)));

  Payload->SetArrayField(TEXT("players"), PlayersArray);

  TArray<TSharedPtr<FJsonValue>> ItemsArray;

  Payload->SetArrayField(TEXT("items"), ItemsArray);

  Payload->SetStringField(TEXT("priorZoneName"), ZoneName);
  Payload->SetStringField(TEXT("zoneName"), InZoneName);

  TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());

  if (!OptionalProxyId.IsEmpty()) {
    Payload->SetStringField(TEXT("proxyId"), OptionalProxyId);
  } else {
    Payload->SetField(TEXT("proxyId"), NullValue);
  }

  Payload->SetBoolField(TEXT("shouldStitch"), bShouldStitch);

  Payload->SetField(TEXT("spawnName"), NullValue);

  TSharedPtr<FJsonObject> Transform = MakeShareable(new FJsonObject);

  TSharedPtr<FJsonObject> Location = MakeShareable(new FJsonObject);
  Location->SetNumberField(TEXT("x"), InTransform.GetLocation().X);
  Location->SetNumberField(TEXT("y"), InTransform.GetLocation().Y);
  Location->SetNumberField(TEXT("z"), InTransform.GetLocation().Z);
  Transform->SetObjectField(TEXT("location"), Location);

  TSharedPtr<FJsonObject> Rotation = MakeShareable(new FJsonObject);
  auto RotationEuler = InTransform.GetRotation().Euler();
  Rotation->SetNumberField(TEXT("x"), RotationEuler.X);
  Rotation->SetNumberField(TEXT("y"), RotationEuler.Y);
  Rotation->SetNumberField(TEXT("z"), RotationEuler.Z);
  Transform->SetObjectField(TEXT("rotation"), Rotation);

  TSharedPtr<FJsonObject> Scale = MakeShareable(new FJsonObject);
  Scale->SetNumberField(TEXT("x"), 0);
  Scale->SetNumberField(TEXT("y"), 0);
  Scale->SetNumberField(TEXT("z"), 0);
  Transform->SetObjectField(TEXT("scale"), Scale);

  Payload->SetObjectField(TEXT("transform"), Transform);

  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Sidecar is not connected; cannot travel player to new zone")
    );
    return;
  }

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Traveling player %s to zone %s"),
    *PlayerId,
    *InZoneName
  );

  Sidecar->Emit(
    TEXT("realm:servers:transfer-zone:game-server-to-sidecar"),
    Payload,
    [this, PlayerId, CharacterId, PlayerController](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));

      if (!Error.IsEmpty()) {
        // kick the player
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Failed to transfer player to new zone, kicking them now: %s"),
          *Error
        );
        GetGameInstance()
          ->GetWorld()
          ->GetAuthGameMode()
          ->GameSession->KickPlayer(PlayerController, FText::FromString(Error));
      }
    }
  );
}

void URedwoodServerGameSubsystem::TravelPlayerToZoneSpawnName(
  APlayerController *PlayerController,
  const FString &InZoneName,
  const FString &InSpawnName,
  const FString &OptionalProxyId
) {
  if (InSpawnName.IsEmpty()) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Cannot travel player to zone %s; provide a non-empty SpawnName"),
      *InZoneName
    );
    return;
  }

  FString UniqueId = PlayerController->PlayerState->GetUniqueId().ToString();

  URedwoodPlayerStateComponent *PlayerStateComponent =
    PlayerController->PlayerState
      ->FindComponentByClass<URedwoodPlayerStateComponent>();

  if (PlayerStateComponent) {
    PlayerStateComponent->InitTransferring();
  }

  FString PlayerId = UniqueId.Left(UniqueId.Find(TEXT(":")));
  FString CharacterId = UniqueId.RightChop(UniqueId.Find(TEXT(":")) + 1);

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());

  TArray<TSharedPtr<FJsonValue>> PlayersArray;
  TSharedPtr<FJsonObject> PlayerObject = MakeShareable(new FJsonObject);

  PlayerObject->SetStringField(TEXT("characterId"), CharacterId);

  TSharedPtr<FJsonObject> TransformOffset = MakeShareable(new FJsonObject);
  TSharedPtr<FJsonObject> VectorOffset = MakeShareable(new FJsonObject);
  VectorOffset->SetNumberField(TEXT("x"), 0);
  VectorOffset->SetNumberField(TEXT("y"), 0);
  VectorOffset->SetNumberField(TEXT("z"), 0);
  TransformOffset->SetObjectField(TEXT("location"), VectorOffset);
  TransformOffset->SetObjectField(TEXT("rotation"), VectorOffset);
  TransformOffset->SetObjectField(TEXT("controlRotation"), VectorOffset);
  PlayerObject->SetObjectField(TEXT("transformOffset"), TransformOffset);

  PlayersArray.Add(MakeShareable(new FJsonValueObject(PlayerObject)));

  Payload->SetArrayField(TEXT("players"), PlayersArray);

  TArray<TSharedPtr<FJsonValue>> ItemsArray;

  Payload->SetArrayField(TEXT("items"), ItemsArray);

  Payload->SetStringField(TEXT("priorZoneName"), ZoneName);
  Payload->SetStringField(TEXT("zoneName"), InZoneName);

  if (!OptionalProxyId.IsEmpty()) {
    Payload->SetStringField(TEXT("proxyId"), OptionalProxyId);
  } else {
    Payload->SetField(TEXT("proxyId"), NullValue);
  }

  Payload->SetStringField(TEXT("spawnName"), InSpawnName);
  Payload->SetField(TEXT("transform"), NullValue);

  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Sidecar is not connected; cannot travel player to new zone")
    );
    return;
  }

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Traveling player %s to zone %s"),
    *PlayerId,
    *InZoneName
  );

  Sidecar->Emit(
    TEXT("realm:servers:transfer-zone:game-server-to-sidecar"),
    Payload,
    [this, PlayerId, CharacterId, PlayerController](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));

      if (!Error.IsEmpty()) {
        // kick the player
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Failed to transfer player to new zone, kicking them now: %s"),
          *Error
        );
        GetGameInstance()
          ->GetWorld()
          ->GetAuthGameMode()
          ->GameSession->KickPlayer(PlayerController, FText::FromString(Error));
      }
    }
  );
}

void URedwoodServerGameSubsystem::FlushSync() {
  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Sidecar is not connected; cannot flush sync data")
    );
    return;
  }

  double CurrentTime = FPlatformTime::Seconds();

  TArray<TSharedPtr<FJsonValue>> ItemsArray;
  for (auto It = SyncItemComponentsById.CreateIterator(); It; ++It) {
    TWeakObjectPtr<URedwoodSyncComponent> &WeakComp = It.Value();

    bool bCleanupEntry = false;
    URedwoodSyncComponent *SyncItemComponent = WeakComp.Get();

    if (SyncItemComponent == nullptr) {
      // item was destroyed
      TSharedPtr<FJsonObject> ItemObject = MakeShareable(new FJsonObject);
      ItemObject->SetStringField(TEXT("id"), It.Key());
      ItemObject->SetBoolField(TEXT("destroyed"), true);
      ItemsArray.Add(MakeShareable(new FJsonValueObject(ItemObject)));

      It.RemoveCurrent(); // remove the current pair safely

      continue;
    }

    if (SyncItemComponent->ZoneName != ZoneName && SyncItemComponent->RedwoodId != TEXT("proxy")) {
      // don't flush items that this server isn't responsible for controlling at all
      continue;
    }

    bool bSyncMovement = false;
    bool bSyncData = SyncItemComponent->IsDataDirty(false);

    if (SyncItemComponent->IsMovementDirty(false) || SyncItemComponent->MovementSyncIntervalSeconds == 0) {
      bSyncMovement = true;
    } else {
      if (SyncItemComponent->MovementSyncIntervalSeconds > 0) {
        if (CurrentTime - SyncItemComponent->GetLastMovementSyncTime() <=
            SyncItemComponent->MovementSyncIntervalSeconds) {
          bSyncMovement = true;
        }
      }
    }

    if (bSyncMovement || bSyncData) {
      TSharedPtr<FJsonObject> ItemObject = MakeShareable(new FJsonObject);
      ItemObject->SetStringField(TEXT("id"), SyncItemComponent->RedwoodId);

      if (bSyncMovement) {
        TSharedPtr<FJsonObject> MovementObject = MakeShareable(new FJsonObject);

        FTransform Transform = SyncItemComponent->GetOwner()->GetTransform();
        FVector Location = Transform.GetLocation();
        FVector Rotation = Transform.GetRotation().Euler();
        FVector Scale = Transform.GetScale3D();

        TSharedPtr<FJsonObject> TransformObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> LocationObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> RotationObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> ScaleObject = MakeShareable(new FJsonObject());
        LocationObject->SetNumberField(TEXT("x"), Location.X);
        LocationObject->SetNumberField(TEXT("y"), Location.Y);
        LocationObject->SetNumberField(TEXT("z"), Location.Z);
        RotationObject->SetNumberField(TEXT("x"), Rotation.X);
        RotationObject->SetNumberField(TEXT("y"), Rotation.Y);
        RotationObject->SetNumberField(TEXT("z"), Rotation.Z);
        ScaleObject->SetNumberField(TEXT("x"), Scale.X);
        ScaleObject->SetNumberField(TEXT("y"), Scale.Y);
        ScaleObject->SetNumberField(TEXT("z"), Scale.Z);
        TransformObject->SetObjectField(TEXT("location"), LocationObject);
        TransformObject->SetObjectField(TEXT("rotation"), RotationObject);
        TransformObject->SetObjectField(TEXT("scale"), ScaleObject);
        MovementObject->SetObjectField(TEXT("transform"), TransformObject);

        ItemObject->SetObjectField(TEXT("movement"), MovementObject);

        SyncItemComponent->SetLastMovementSyncTime(CurrentTime);
      }

      if (bSyncData) {
        USIOJsonObject *DataObject =
          URedwoodCommonGameSubsystem::SerializeBackendData(
            SyncItemComponent->bStoreDataInActor
              ? (UObject *)SyncItemComponent->GetOwner()
              : (UObject *)SyncItemComponent,
            SyncItemComponent->DataVariableName
          );
        ItemObject->SetObjectField(TEXT("data"), DataObject->GetRootObject());
      }

      ItemsArray.Add(MakeShareable(new FJsonValueObject(ItemObject)));

      SyncItemComponent->ClearDirtyFlags(false);
    }
  }

  if (ItemsArray.Num() > 0) {
    TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

    Payload->SetArrayField(TEXT("items"), ItemsArray);

    Sidecar->Emit(TEXT("realm:servers:session:sync:batch"), Payload);
  }
}

void URedwoodServerGameSubsystem::FlushPersistence() {
  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(
      LogRedwood, Error, TEXT("Can't FlushPersistence: World is not valid")
    );
    return;
  }

  AGameStateBase *GameState = World->GetGameState();
  if (!IsValid(GameState)) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Can't FlushPlayerCharacterData: GameState is not valid")
    );
  } else {
    FlushPlayerCharacterData(GameState->PlayerArray, false);
  }

  FlushZoneData();
}

void URedwoodServerGameSubsystem::FlushPlayerCharacterData(
  TArray<APlayerState *> PlayerArray, bool bForce
) {
  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Can't FlushPlayerCharacterData: World is not valid")
    );
    return;
  }

  bool bUseBackend = URedwoodCommonGameSubsystem::ShouldUseBackend(World);

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());

  FString SavePath =
    FPaths::ProjectSavedDir() / TEXT("Persistence") / TEXT("Characters");
  FPaths::NormalizeDirectoryName(SavePath);

  if (!bUseBackend) {
    IFileManager::Get().MakeDirectory(*SavePath, true);
  }

  TArray<TSharedPtr<FJsonValue>> CharactersArray;
  for (TObjectPtr<APlayerState> PlayerState : PlayerArray) {
    TSharedPtr<FJsonObject> CharacterObject =
      CreatePlayerCharacterDataObject(PlayerState, bForce);

    if (CharacterObject.IsValid()) {
      TSharedPtr<FJsonValueObject> Value =
        MakeShareable(new FJsonValueObject(CharacterObject));
      CharactersArray.Add(Value);

      if (!bUseBackend) {
        // save to disk
        CharacterObject->SetStringField(
          TEXT("id"), CharacterObject->GetStringField(TEXT("characterId"))
        );
        URedwoodCommonGameSubsystem::SaveCharacterJsonToDisk(CharacterObject);
      }
    }
  }

  Payload->SetArrayField(TEXT("characters"), CharactersArray);
  Payload->SetStringField(TEXT("id"), TEXT("game-server"));

  if (bUseBackend && CharactersArray.Num() > 0) {
    Sidecar->Emit(TEXT("realm:characters:set:server"), Payload);
  }
}

// FORK(hollowed-oath): fork-added function (with EmitPlayerLeft below), not
// in upstream Redwood — linkdead pawn retention support. See the header for
// the contract; keep both across upstream merges.
void URedwoodServerGameSubsystem::FlushDetachedCharacterData(
  APawn *Pawn,
  const FString &CharacterId,
  const FString &PlayerId,
  bool bReleaseBindingWhenSettled
) {
  UWorld *World = GetWorld();
  if (!IsValid(World) || CharacterId.IsEmpty()) {
    return;
  }

  bool bUseBackend = URedwoodCommonGameSubsystem::ShouldUseBackend(World);

  // Without the sidecar nothing can be delivered — neither the flush nor the
  // binding-releasing player-left. Bail before serializing anything and
  // WITHOUT clearing dirty flags, so the unsent data stays flushable.
  if (bUseBackend && (!Sidecar.IsValid() || !Sidecar->bIsConnected)) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT(
        "FlushDetachedCharacterData: sidecar not connected; dropping flush%s for character %s"
      ),
      bReleaseBindingWhenSettled ? TEXT(" and binding release") : TEXT(""),
      *CharacterId
    );
    return;
  }

  // Skip paths that put no write in flight have nothing to order the release
  // behind, so the caller-requested binding release can go out immediately.
  auto ReleaseBindingNow =
    [this, &PlayerId, &CharacterId, bReleaseBindingWhenSettled]() {
      if (bReleaseBindingWhenSettled) {
        EmitPlayerLeft(PlayerId, CharacterId);
      }
    };

  if (!IsValid(Pawn)) {
    ReleaseBindingNow();
    return;
  }

  URedwoodCharacterComponent *CharacterComponent =
    Pawn->FindComponentByClass<URedwoodCharacterComponent>();
  if (!IsValid(CharacterComponent)) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "FlushDetachedCharacterData: pawn %s has no URedwoodCharacterComponent"
      ),
      *Pawn->GetName()
    );
    ReleaseBindingNow();
    return;
  }

  // Same safety rail as the PlayerState flush: refuse to persist a component
  // whose character identity doesn't match the id we're flushing for.
  if (CharacterComponent->RedwoodCharacterId != CharacterId) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "FlushDetachedCharacterData: pawn %s carries character id '%s', expected '%s'; skipping"
      ),
      *Pawn->GetName(),
      *CharacterComponent->RedwoodCharacterId,
      *CharacterId
    );
    ReleaseBindingNow();
    return;
  }

  TSharedPtr<FJsonObject> CharacterObject = MakeShareable(new FJsonObject);
  CharacterObject->SetStringField(TEXT("playerId"), PlayerId);
  CharacterObject->SetStringField(TEXT("characterId"), CharacterId);

  AActor *ComponentOwner = CharacterComponent->GetOwner();
  int32 SerializedFieldCount = 0;
  auto SerializeField = [&](
                          bool bUseField,
                          bool bFieldDirty,
                          const FString &VariableName,
                          const TCHAR *FieldName
                        ) {
    // Backend saves merge per-field, so send only groups gameplay actually
    // dirtied since the last flush — a whole-object save could overwrite
    // newer backend values written while the pawn was detached (e.g. the
    // player editing their character through the frontend). Offline disk
    // saves replace the whole document, so those serialize every enabled
    // group (same split as FlushPlayerCharacterData).
    if (!bUseField || (bUseBackend && !bFieldDirty)) {
      return;
    }
    USIOJsonObject *Value = URedwoodCommonGameSubsystem::SerializeBackendData(
      CharacterComponent->bStoreDataInActor ? (UObject *)ComponentOwner
                                            : (UObject *)CharacterComponent,
      VariableName
    );
    if (Value) {
      CharacterObject->SetObjectField(FieldName, Value->GetRootObject());
      SerializedFieldCount++;
    }
  };

  SerializeField(
    CharacterComponent->bUseCharacterCreatorData,
    CharacterComponent->IsCharacterCreatorDataDirty(),
    CharacterComponent->CharacterCreatorDataVariableName,
    TEXT("characterCreatorData")
  );
  SerializeField(
    CharacterComponent->bUseMetadata,
    CharacterComponent->IsMetadataDirty(),
    CharacterComponent->MetadataVariableName,
    TEXT("metadata")
  );
  SerializeField(
    CharacterComponent->bUseEquippedInventory,
    CharacterComponent->IsEquippedInventoryDirty(),
    CharacterComponent->EquippedInventoryVariableName,
    TEXT("equippedInventory")
  );
  SerializeField(
    CharacterComponent->bUseNonequippedInventory,
    CharacterComponent->IsNonequippedInventoryDirty(),
    CharacterComponent->NonequippedInventoryVariableName,
    TEXT("nonequippedInventory")
  );
  SerializeField(
    CharacterComponent->bUseProgress,
    CharacterComponent->IsProgressDirty(),
    CharacterComponent->ProgressVariableName,
    TEXT("progress")
  );
  SerializeField(
    CharacterComponent->bUseData,
    CharacterComponent->IsDataDirty(),
    CharacterComponent->DataVariableName,
    TEXT("data")
  );
  SerializeField(
    CharacterComponent->bUseAbilitySystem,
    CharacterComponent->IsAbilitySystemDirty(),
    CharacterComponent->AbilitySystemVariableName,
    TEXT("abilitySystem")
  );

  // FORK(hollowed-oath) BEGIN: detached-flush item leg + settled-latch release barrier. Closes the
  // gap where a retained linkdead pawn's dirty item rows were SILENTLY DROPPED -- the container
  // channel this replaced (RedwoodPlugins#17) was never wired into FlushDetachedCharacterData, so a
  // pawn that outlived its player lost every inventory mutation made after the last live flush.
  // Items must not.
  //
  // Placed AFTER the seven-field serialize (so it is NOT gated by the SerializedFieldCount == 0
  // case below -- a pawn whose only change was its inventory has zero dirty character-blob fields
  // yet still carries dirty items).
  //
  // ORDERING HAZARD (why the latch): in backend mode both the item flush and the set:server write
  // are async sidecar round trips, and the backend's player-left (EmitPlayerLeft) tears down the
  // character->instance write binding that each write's auth gate still needs -- immediately,
  // deleting the online/bound state. This barrier is NEWLY reachable here: the old container
  // channel never ran in this function, so the item leg is the first async write it fires, and an
  // unchained release could land BEFORE realm:characters:items:flush is processed, making the auth
  // gate reject the flush and drop the very detached rows this leg exists to save. So when a
  // release is requested it MUST wait until BOTH pending writes have SETTLED. This settled-latch is
  // the interim barrier until the full flush-barrier work lands (game Plan C / #1365 family).
  if (!bUseBackend) {
    // Offline: item rows go inline into the on-disk character JSON, mirroring
    // CreatePlayerCharacterDataObject's offline leg (a bare FlushItemsForCharacterComponent would
    // no-op with no sidecar and drop the rows). No barrier needed -- the disk write is synchronous
    // and the binding release is a no-op offline (EmitPlayerLeft short-circuits without a backend).
    TArray<TSharedPtr<FJsonValue>> DetachedOfflineItemRows;
    if (AppendOfflineItemRows(DetachedOfflineItemRows, CharacterComponent)) {
      CharacterObject->SetArrayField(TEXT("items"), DetachedOfflineItemRows);
    }
    CharacterObject->SetStringField(TEXT("id"), CharacterId);
    URedwoodCommonGameSubsystem::SaveCharacterJsonToDisk(CharacterObject);
    CharacterComponent->ClearDirtyFlags();
    ReleaseBindingNow();
    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Flushed detached character data for character %s"),
      *CharacterId
    );
    return;
  }

  // Backend mode. Count the async writes the binding release must wait behind.
  const bool bItemsLegPending = CharacterComponent->IsItemsDirty();
  const bool bSetServerPending = SerializedFieldCount > 0;
  const int32 PendingSettles =
    (bItemsLegPending ? 1 : 0) + (bSetServerPending ? 1 : 0);

  if (PendingSettles == 0) {
    // Nothing dirty at all (no items, no character-blob fields): no async write to order behind, so
    // release immediately, exactly as before.
    UE_LOG(
      LogRedwood,
      Verbose,
      TEXT(
        "FlushDetachedCharacterData: nothing dirty for character %s; skipping"
      ),
      *CharacterId
    );
    ReleaseBindingNow();
    return;
  }

  // Settled-latch: each pending write decrements it by one; the last one to reach zero runs the
  // release continuation. Shared by value into every async callback so it outlives this stack
  // frame. "Settled" means the write attempt RESOLVED (success OR error), NOT that it committed --
  // an errored write still can't be raced by the release, and the linkdead body must not be pinned
  // forever waiting on a settle that will never upgrade to a commit.
  TSharedRef<int32> SettleLatch = MakeShared<int32>(PendingSettles);
  TWeakObjectPtr<URedwoodServerGameSubsystem> WeakThis(this);
  auto SettleOne =
    [WeakThis, SettleLatch, PlayerId, CharacterId, bReleaseBindingWhenSettled]() {
      if (--(*SettleLatch) > 0) {
        return;
      }
      // Last pending write settled -- releasing the binding now can no longer beat a write's auth
      // gate. Gated on the caller's request; when not requested this is a bare countdown that fires
      // nothing.
      if (bReleaseBindingWhenSettled && WeakThis.IsValid()) {
        WeakThis->EmitPlayerLeft(PlayerId, CharacterId);
      }
    };

  // FORK(hollowed-oath): stranding hazard at THIS release site, not just the park path it depends
  // on. If FlushItemsForCharacterComponent finds a batch already in flight it PARKS this attempt
  // (see the single-flight comment in that function) instead of sending it, and still calls
  // SettleOne synchronously -- so the latch above can reach zero and EmitPlayerLeft can release the
  // write binding while this pawn's dirty items never actually left the process. A live, still-
  // ticking component would retry on its next flush; a detached pawn has none once the binding is
  // released. This settled-latch does not close that gap -- it only orders the release behind an
  // attempt SETTLING, not behind the items actually landing. #1365's flush-barrier work is what
  // closes it (by keeping the pawn's binding alive until a real delivery, not just a settle).
  // Item flush leg. PlayerStateComponent is null here (a detached pawn has no live PlayerState), so
  // FlushItemsForCharacterComponent sources characterId from the component's RedwoodCharacterId,
  // already verified equal to CharacterId at the top of this function. Single-flight makes it safe
  // against a concurrent tick flush. OnAckSettled decrements the latch when the flush settles (or
  // synchronously if it sends nothing) -- guaranteed exactly once, so the latch can never wedge.
  if (bItemsLegPending) {
    FlushItemsForCharacterComponent(nullptr, CharacterComponent, SettleOne);
  }

  if (bSetServerPending) {
    TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
    TArray<TSharedPtr<FJsonValue>> CharactersArray;
    CharactersArray.Add(MakeShareable(new FJsonValueObject(CharacterObject)));
    Payload->SetArrayField(TEXT("characters"), CharactersArray);
    Payload->SetStringField(TEXT("id"), TEXT("game-server"));
    // Route the set:server ack through the SAME latch. This preserves the prior "chain the release
    // on the flush ack" intent -- same-socket emission order does NOT serialize the backend's async
    // handlers, so a player-left emitted right after this write could be processed first and release
    // the binding it still needs -- and now additionally waits behind the item flush. When no
    // release is requested SettleOne is just a countdown. (Fire-and-forget error handling is
    // unchanged: neither the old nor this callback inspects the ack beyond ordering.)
    Sidecar->Emit(
      TEXT("realm:characters:set:server"),
      Payload,
      [SettleOne](auto Response) {
        SettleOne();
      }
    );
    // Clear only once a write is actually in flight: clearing before a dropped emit would silently
    // lose the data forever (nothing would ever look dirty again). Items ride their own ack
    // (CompleteItemFlush), so ClearDirtyFlags deliberately doesn't touch item dirty state.
    CharacterComponent->ClearDirtyFlags();
  }
  // FORK(hollowed-oath) END

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Flushed detached character data for character %s"),
    *CharacterId
  );
}

// FORK(hollowed-oath): fork-added function — see FlushDetachedCharacterData.
void URedwoodServerGameSubsystem::EmitPlayerLeft(
  const FString &PlayerId, const FString &CharacterId
) {
  UWorld *World = GetWorld();
  if (!IsValid(World) || !URedwoodCommonGameSubsystem::ShouldUseBackend(World)) {
    return;
  }

  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("EmitPlayerLeft: sidecar not connected; dropping for character %s"),
      *CharacterId
    );
    return;
  }

  TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
  JsonObject->SetStringField(TEXT("playerId"), PlayerId);
  JsonObject->SetStringField(TEXT("characterId"), CharacterId);
  Sidecar->Emit(
    TEXT("realm:servers:player-left:game-server-to-sidecar"), JsonObject
  );
}

TSharedPtr<FJsonObject>
URedwoodServerGameSubsystem::CreatePlayerCharacterDataObject(
  APlayerState *PlayerState, bool bForce
) {
  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Can't CreatePlayerCharacterDataObject: World is not valid")
    );
    return TSharedPtr<FJsonObject>();
  }

  bool bUseBackend = URedwoodCommonGameSubsystem::ShouldUseBackend(World);

  URedwoodPlayerStateComponent *PlayerStateComponent =
    PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();

  if (IsValid(PlayerStateComponent)) {
    TArray<URedwoodCharacterComponent *> CharacterComponents;
    PlayerState->GetComponents<URedwoodCharacterComponent>(CharacterComponents);

    APawn *Pawn = PlayerState->GetPawn();
    if (Pawn) {
      TArray<URedwoodCharacterComponent *> PawnCharacterComponents;
      Pawn->GetComponents<URedwoodCharacterComponent>(PawnCharacterComponents);
      CharacterComponents.Append(PawnCharacterComponents);
    }

    TArray<TScriptInterface<IRedwoodPersistenceComponentInterface>>
      PersistenceComponents;

    TArray<UActorComponent *> AllComponents;
    PlayerState->GetComponents<UActorComponent>(AllComponents);
    if (Pawn) {
      TArray<UActorComponent *> PawnComponents;
      Pawn->GetComponents<UActorComponent>(PawnComponents);
      AllComponents.Append(PawnComponents);
    }

    for (UActorComponent *Component : AllComponents) {
      if (Component->Implements<URedwoodPersistenceComponentInterface>()) {
        PersistenceComponents.Add(
          TScriptInterface<IRedwoodPersistenceComponentInterface>(Component)
        );
      }
    }

    UE_LOG(
      LogRedwood,
      VeryVerbose,
      TEXT("Flushing character %s"),
      *PlayerStateComponent->RedwoodCharacter.Name
    );

    bool bIsDirty = false;
    TSharedPtr<FJsonObject> CharacterObject = MakeShareable(new FJsonObject);
    CharacterObject->SetStringField(
      TEXT("playerId"), *PlayerStateComponent->RedwoodCharacter.PlayerId
    );
    CharacterObject->SetStringField(
      TEXT("characterId"), *PlayerStateComponent->RedwoodCharacter.Id
    );

    if (!PlayerStateComponent->bTransferring && Pawn) {
      APlayerController *PlayerController =
        Pawn->GetController<APlayerController>();

      if (PlayerController) {
        TSharedPtr<FJsonObject> LastLocationObject =
          MakeShareable(new FJsonObject);

        LastLocationObject->SetStringField(TEXT("proxyId"), ProxyId);
        LastLocationObject->SetStringField(TEXT("zoneName"), ZoneName);

        TSharedPtr<FJsonObject> TransformObject =
          MakeShareable(new FJsonObject);
        TSharedPtr<FJsonObject> LocationObject = MakeShareable(new FJsonObject);
        FVector ActorLocation = Pawn->GetActorLocation();
        LocationObject->SetNumberField(TEXT("x"), ActorLocation.X);
        LocationObject->SetNumberField(TEXT("y"), ActorLocation.Y);
        LocationObject->SetNumberField(TEXT("z"), ActorLocation.Z);
        TransformObject->SetObjectField(TEXT("location"), LocationObject);

        TSharedPtr<FJsonObject> RotationObject = MakeShareable(new FJsonObject);
        FVector ActorRotation = Pawn->GetActorRotation().Euler();
        RotationObject->SetNumberField(TEXT("x"), ActorRotation.X);
        RotationObject->SetNumberField(TEXT("y"), ActorRotation.Y);
        RotationObject->SetNumberField(TEXT("z"), ActorRotation.Z);
        TransformObject->SetObjectField(TEXT("rotation"), RotationObject);

        TSharedPtr<FJsonObject> ControlRotationObject =
          MakeShareable(new FJsonObject);
        FRotator ControlRotation = PlayerController->GetControlRotation();
        ControlRotationObject->SetNumberField(TEXT("x"), ControlRotation.Roll);
        ControlRotationObject->SetNumberField(TEXT("y"), ControlRotation.Pitch);
        ControlRotationObject->SetNumberField(TEXT("z"), ControlRotation.Yaw);
        TransformObject->SetObjectField(
          TEXT("controlRotation"), ControlRotationObject
        );

        LastLocationObject->SetObjectField(TEXT("transform"), TransformObject);

        CharacterObject->SetObjectField(
          TEXT("lastLocation"), LastLocationObject
        );

        bIsDirty = true;
      }
    }

    // If the PlayerStateComponent's dirty flag is set, use that data
    // instead of the character component and broadcast the data changed
    // so character components will sync with the new data
    if (PlayerStateComponent->IsCharacterDataDirty()) {
      if (
        PlayerStateComponent->RedwoodCharacter.CharacterCreatorData != nullptr &&
        PlayerStateComponent->RedwoodCharacter.CharacterCreatorData->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("characterCreatorData"),
          PlayerStateComponent->RedwoodCharacter.CharacterCreatorData
            ->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.Metadata != nullptr &&
        PlayerStateComponent->RedwoodCharacter.Metadata->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("metadata"),
          PlayerStateComponent->RedwoodCharacter.Metadata->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.EquippedInventory != nullptr &&
        PlayerStateComponent->RedwoodCharacter.EquippedInventory->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("equippedInventory"),
          PlayerStateComponent->RedwoodCharacter.EquippedInventory
            ->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.NonequippedInventory != nullptr &&
        PlayerStateComponent->RedwoodCharacter.NonequippedInventory->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("nonequippedInventory"),
          PlayerStateComponent->RedwoodCharacter.NonequippedInventory
            ->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.Progress != nullptr &&
        PlayerStateComponent->RedwoodCharacter.Progress->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("progress"),
          PlayerStateComponent->RedwoodCharacter.Progress->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.Data != nullptr &&
        PlayerStateComponent->RedwoodCharacter.Data->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("data"),
          PlayerStateComponent->RedwoodCharacter.Data->GetRootObject()
        );
      }

      if (
        PlayerStateComponent->RedwoodCharacter.AbilitySystem != nullptr &&
        PlayerStateComponent->RedwoodCharacter.AbilitySystem->IsValid()
      ) {
        CharacterObject->SetObjectField(
          TEXT("abilitySystem"),
          PlayerStateComponent->RedwoodCharacter.AbilitySystem->GetRootObject()
        );
      }

      // FORK(hollowed-oath) BEGIN: offline item carry-across in the "PlayerState snapshot is
      // authoritative" branch (fork-added; this whole !bUseBackend inline-items write has no
      // upstream counterpart). Retargeted from the earlier per-container write
      // (RedwoodCharacter.Containers / SerializeContainerRecords, RedwoodPlugins#17). Merge must
      // keep it gated on !bUseBackend -- the backend leg's rows live in the Item table and must NOT
      // be folded into realm:characters:set:server.
      // This branch treats the PlayerState's own character snapshot as authoritative and skips
      // the character components entirely, so offline it must carry the snapshot's item rows
      // across too -- the disk save is a whole-file rewrite, and omitting the key here would drop
      // every item row the character had. The backend leg needs nothing: its rows live in
      // the Item table, untouched by realm:characters:set:server.
      if (!bUseBackend) {
        CharacterObject->SetArrayField(
          TEXT("items"),
          URedwoodCommonGameSubsystem::SerializeItemRecords(
            PlayerStateComponent->RedwoodCharacter.Items
          )
        );
      }
      // FORK(hollowed-oath) END

      PlayerStateComponent->OnRedwoodCharacterUpdated.Broadcast();

      bIsDirty = true;
    } else {
      // FORK(hollowed-oath): offline-only accumulator for the union of every character component's
      // item rows (written once after the loop, below). Fork-added -- the backend leg sends
      // each component's rows on its own sidecar channel and needs no accumulator.
      // Offline only: the union of every character component's item rows, written to the
      // single "items" field after the loop below (the backend leg sends each component's
      // rows on its own sidecar channel instead, so it needs no accumulator).
      TArray<TSharedPtr<FJsonValue>> OfflineItemRows;
      bool bHasOfflineItemRows = false;

      for (URedwoodCharacterComponent *CharacterComponent :
           CharacterComponents) {
        if (bUseBackend && // always fill out if not using the backend
              !CharacterComponent->IsCharacterCreatorDataDirty() &&
              !CharacterComponent->IsMetadataDirty() &&
              !CharacterComponent->IsEquippedInventoryDirty() &&
              !CharacterComponent->IsNonequippedInventoryDirty() &&
              !CharacterComponent->IsProgressDirty() &&
              !CharacterComponent->IsDataDirty() &&
              !CharacterComponent->IsAbilitySystemDirty() &&
              // FORK(hollowed-oath): items added to the "nothing dirty, skip this component"
              // guard so an item-only change still forces a flush. Keep in the AND-chain.
              !CharacterComponent->IsItemsDirty()) {
          continue;
        }

        // Safety rail: refuse to serialize a component whose character identity
        // doesn't match the PlayerState's authoritative character. This stops a
        // stale or un-hydrated pawn-owned URedwoodCharacterComponent from ever
        // contributing empty/default field values to the backend save (which is
        // how prior pawn-possession flows could wipe a character row).
        const FString& ComponentCharId = CharacterComponent->RedwoodCharacterId;
        const FString& AuthoritativeCharId = PlayerStateComponent->RedwoodCharacter.Id;
        if (ComponentCharId.IsEmpty() || ComponentCharId != AuthoritativeCharId) {
          UE_LOG(
            LogRedwood,
            Warning,
            TEXT("Skipping save for RedwoodCharacterComponent on %s: character id '%s' does not match PlayerState character id '%s' (pawn %s)"),
            CharacterComponent->GetOwner() ? *CharacterComponent->GetOwner()->GetName() : TEXT("<null>"),
            *ComponentCharId,
            *AuthoritativeCharId,
            Pawn ? *Pawn->GetName() : TEXT("<none>")
          );
          CharacterComponent->ClearDirtyFlags();
          // FORK(hollowed-oath): item dirty state is NOT touched by ClearDirtyFlags (it clears
          // only on a send's ack), so the identity-mismatch skip path must clear it explicitly or
          // this component's items stay dirty forever. Fork-added; keep alongside the skip.
          // ClearDirtyFlags() deliberately leaves items alone (they only clear on a
          // send's ack) -- but this component is never going to be flushed at all (it's
          // being skipped outright), so there is no in-flight send to race; clear its
          // item dirty state directly instead of leaving it stuck forever.
          CharacterComponent->ClearItemsDirtyState();
          continue;
        }

        bIsDirty = true;

        AActor *ComponentOwner = CharacterComponent->GetOwner();

        if (bForce || bUseBackend ? CharacterComponent->IsCharacterCreatorDataDirty()
                          : CharacterComponent->bUseCharacterCreatorData) {
          USIOJsonObject *CharacterCreatorData =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->CharacterCreatorDataVariableName
            );
          if (CharacterCreatorData) {
            PlayerStateComponent->RedwoodCharacter.CharacterCreatorData =
              CharacterCreatorData;
            CharacterObject->SetObjectField(
              TEXT("characterCreatorData"),
              CharacterCreatorData->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsMetadataDirty()
                          : CharacterComponent->bUseMetadata) {
          USIOJsonObject *Metadata =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->MetadataVariableName
            );
          if (Metadata) {
            PlayerStateComponent->RedwoodCharacter.Metadata = Metadata;
            CharacterObject->SetObjectField(
              TEXT("metadata"), Metadata->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsEquippedInventoryDirty()
                          : CharacterComponent->bUseEquippedInventory) {
          USIOJsonObject *EquippedInventory =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->EquippedInventoryVariableName
            );
          if (EquippedInventory) {
            PlayerStateComponent->RedwoodCharacter.EquippedInventory =
              EquippedInventory;
            CharacterObject->SetObjectField(
              TEXT("equippedInventory"), EquippedInventory->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsNonequippedInventoryDirty()
                          : CharacterComponent->bUseNonequippedInventory) {
          USIOJsonObject *NonequippedInventory =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->NonequippedInventoryVariableName
            );
          if (NonequippedInventory) {
            PlayerStateComponent->RedwoodCharacter.NonequippedInventory =
              NonequippedInventory;
            CharacterObject->SetObjectField(
              TEXT("nonequippedInventory"),
              NonequippedInventory->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsProgressDirty()
                          : CharacterComponent->bUseProgress) {
          USIOJsonObject *Progress =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->ProgressVariableName
            );
          if (Progress) {
            PlayerStateComponent->RedwoodCharacter.Progress = Progress;
            CharacterObject->SetObjectField(
              TEXT("progress"), Progress->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsDataDirty() : CharacterComponent->bUseData) {
          USIOJsonObject *CharData =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->DataVariableName
            );
          if (CharData) {
            PlayerStateComponent->RedwoodCharacter.Data = CharData;
            CharacterObject->SetObjectField(
              TEXT("data"), CharData->GetRootObject()
            );
          }
        }

        if (bForce || bUseBackend ? CharacterComponent->IsAbilitySystemDirty()
                          : CharacterComponent->bUseAbilitySystem) {
          USIOJsonObject *AbilitySystem =
            URedwoodCommonGameSubsystem::SerializeBackendData(
              CharacterComponent->bStoreDataInActor
                ? (UObject *)ComponentOwner
                : (UObject *)CharacterComponent,
              CharacterComponent->AbilitySystemVariableName
            );
          if (AbilitySystem) {
            PlayerStateComponent->RedwoodCharacter.AbilitySystem =
              AbilitySystem;
            CharacterObject->SetObjectField(
              TEXT("abilitySystem"), AbilitySystem->GetRootObject()
            );
          }
        }

        // FORK(hollowed-oath) BEGIN: per-component item flush dispatch. Fork-added; upstream
        // has neither branch. Backend leg routes dirty rows out on their OWN sidecar channel
        // (FlushItemsForCharacterComponent) and deliberately does NOT fold "items" into
        // the realm:characters:set:server payload. Offline leg appends the rows into
        // CharacterObject for the on-disk JSON. Merge must preserve the bUseBackend split and keep
        // this BEFORE ClearDirtyFlags (which does not clear item state -- the flush's ack /
        // the offline append do).
        // Items are NOT folded into CharacterObject when it's headed for
        // realm:characters:set:server -- that payload's schema has no "items" field: items live in
        // their own Item table + sidecar routes, deliberately outside the single-row PlayerCharacter
        // blob so a one-item change doesn't rewrite every other item. Send them out on their own
        // channel instead; the flush's ack clears the dirty state.
        //
        // Offline/PIE has neither that route nor an Item table: CharacterObject IS the
        // character's JSON file (FlushPlayerCharacterData writes it straight to disk), so the
        // rows go inline there instead. Without this the file would carry only the legacy flat
        // nonequippedInventory blob and every load would degrade to the legacy split path.
        if (bUseBackend) {
          if (CharacterComponent->IsItemsDirty()) {
            FlushItemsForCharacterComponent(
              PlayerStateComponent, CharacterComponent
            );
          }
        } else if (AppendOfflineItemRows(
                     OfflineItemRows, CharacterComponent
                   )) {
          bHasOfflineItemRows = true;
        }
        // FORK(hollowed-oath) END

        CharacterComponent->ClearDirtyFlags();
      }

      // FORK(hollowed-oath): write the accumulated offline item rows once (fork-added). Guarded
      // so a game not using items keeps the field absent rather than gaining an empty array.
      // One "items" field, written once from every component's rows -- see
      // AppendOfflineItemRows. Only written when a component actually owns items, so a
      // game not using them keeps the field absent rather than gaining an empty array.
      if (bHasOfflineItemRows) {
        CharacterObject->SetArrayField(
          TEXT("items"), OfflineItemRows
        );
      }
    }

    for (TScriptInterface<IRedwoodPersistenceComponentInterface>
           ComponentInterface : PersistenceComponents) {
      IRedwoodPersistenceComponentInterface *PersistenceComponent =
        ComponentInterface.GetInterface();
      if (PersistenceComponent) {
        if (PersistenceComponent->AddPersistedData(CharacterObject, bForce)) {
          bIsDirty = true;
        }
      }
    }

    if (!bIsDirty) {
      return TSharedPtr<FJsonObject>();
    }

    return CharacterObject;
  } else {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("PlayerState does not have a URedwoodPlayerStateComponent")
    );
    return TSharedPtr<FJsonObject>();
  }
}

// FORK(hollowed-oath) BEGIN: item flush + migrate + trade + offline-append helpers, entirely
// fork-added (no upstream counterpart). Replaces the earlier per-container flush helpers
// (FlushContainersForCharacterComponent / AppendOfflineContainerRows, RedwoodPlugins#17) now that
// persistence is per-item flat rows instead of opaque per-container blobs. Backend counterpart:
// RedwoodBackend feat/item-persistence (realm:characters:items:{flush,migrate,trade} routes).
//
// FlushItemsForCharacterComponent is the per-item persistence channel, protocol v2: single-flight
// + seq-fenced. It sends ONLY currently-dirty item records (plus pending deletions) to
// realm:characters:items:flush under a batchSeq claimed by TryBeginItemFlush, and clears dirty
// state ONLY on a generation-matched ack inside CompleteItemFlush, so a re-dirty mid-flight is
// never stranded and a failed/lost/component-destroyed send is always retried. At most one batch
// is outstanding: while one is in flight TryBeginItemFlush returns false and this tick's flush is
// parked (dirt keeps coalescing). AppendOfflineItemRows is its offline/PIE twin (whole-array write
// into the on-disk character JSON). EmitItemsMigrate / EmitItemsTrade are one-shot backend ops
// with no dirty-state interaction. Called from CreatePlayerCharacterDataObject /
// FlushDetachedCharacterData above; the game module (AClient / per-bag inventory) drives the dirty
// state via URedwoodCharacterComponent::MarkItemsDirty/MarkItemsDeleted and calls the migrate/trade
// emits from Plan C. Declarations sit under a matching FORK marker in RedwoodServerGameSubsystem.h.
// Merge must preserve: the single-flight/seq-fence contract, the generation-matched ack, the
// (id, generation) send-set independence from the live maps, and the TWeakObjectPtr guard on every
// async callback.
//
// Sends CharacterComponent's currently-dirty item records (plus pending deletions) to
// realm:characters:items:flush under a claimed batchSeq. Dirty state is cleared ONLY on the ack
// inside CompleteItemFlush (below), and only for the exact (id, generation) pairs this call
// actually sent -- if MarkItemsDirty/MarkItemsDeleted bumped an id's generation again while the
// request was in flight, CompleteItemFlush leaves that id dirty at its newer generation instead of
// clearing it, so the next flush cycle re-sends the newer record rather than a mid-flight re-dirty
// being silently stranded. A failed send leaves the dirty state set (CompleteItemFlush releases
// the single-flight slot but retains the dirt); a component/actor destroyed mid-flight strands
// nothing recoverable and just returns. CompleteItemFlush ALWAYS runs on a live component (success
// or error) so the flight slot is released and the next flush can proceed. TWeakObjectPtr guards
// the async callback the same way the sibling channels' callbacks do.
//
// PlayerStateComponent supplies the payload's characterId; it may be null on the detached-flush leg
// (a retained linkdead pawn has no live PlayerState), in which case the id comes from the
// component's own RedwoodCharacterId, which the dispatch site already verified equal to the
// authoritative id.
void URedwoodServerGameSubsystem::FlushItemsForCharacterComponent(
  URedwoodPlayerStateComponent *PlayerStateComponent,
  URedwoodCharacterComponent *CharacterComponent,
  TFunction<void()> OnAckSettled
) {
  // OnAckSettled must fire EXACTLY ONCE on every path through this function -- the detached-flush
  // release barrier (FlushDetachedCharacterData) counts on it to know this flush attempt has
  // SETTLED before it releases the character's write binding. "Settled" = the in-flight attempt
  // resolved (success OR error), NOT that it committed. Every early-return below settles
  // synchronously via this helper; the sole sending path settles inside the async ack callback.
  // Null for the ordinary tick-flush caller, where this is a no-op.
  auto Settle = [&OnAckSettled]() {
    if (OnAckSettled) {
      OnAckSettled();
    }
  };

  // Early-out before claiming the flight slot: the detached-flush leg calls this without the
  // dispatch site's IsItemsDirty() pre-check, and a slot claimed here would never be released.
  if (!CharacterComponent->IsItemsDirty()) {
    Settle();
    return;
  }

  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood, Error, TEXT("Sidecar is not connected; cannot flush items")
    );
    Settle();
    return;
  }

  // Resolve the records array BEFORE claiming the single-flight slot: a misconfigured
  // ItemsVariableName returns null here, and bailing after TryBeginItemFlush would strand the slot
  // forever (nothing would release bItemFlushInFlight).
  TArray<FRedwoodItemRecord> *RecordsArray =
    URedwoodCommonGameSubsystem::ResolveItemsRecordsArray(CharacterComponent);
  if (!RecordsArray) {
    Settle();
    return;
  }

  // Claim the single-flight slot + the batchSeq this batch must stamp. If a batch is already
  // outstanding, park this tick's flush -- the dirt stays marked and coalesces until the in-flight
  // batch's ack releases the slot. Settle so the detached barrier doesn't wait forever on a parked
  // tick. For a LIVE component the still-dirty rows retry on that component's next tick-flush; a
  // DETACHED pawn has no next tick once FlushDetachedCharacterData's settled-latch reaches zero and
  // releases the write binding, so a park here can strand those rows outright rather than merely
  // delay them -- see the FORK note at that function's item-flush call site. The barrier is interim,
  // not a delivery guarantee, until #1365's flush-barrier work closes this gap.
  int64 BatchSeq = 0;
  if (!CharacterComponent->TryBeginItemFlush(BatchSeq)) {
    Settle();
    return;
  }

  // Generation counters, not plain ids -- lets CompleteItemFlush tell "the record I sent" apart
  // from "the record as it stands after a re-dirty that arrived while this send was in flight".
  const TMap<FString, uint64> &DirtyGenerations =
    CharacterComponent->GetDirtyItemGenerations();
  const TMap<FString, uint64> &DeletedGenerations =
    CharacterComponent->GetPendingDeletedItemGenerations();

  TArray<TSharedPtr<FJsonValue>> UpsertsJsonArray;
  // The exact (id, generation) pairs actually included in UpsertsJsonArray below -- NOT simply
  // DirtyGenerations, since a dirty id with no matching record (logged below) is never sent and
  // must not be acked.
  TArray<TPair<FString, uint64>> SentUpserts;
  for (const FRedwoodItemRecord &Record : *RecordsArray) {
    const uint64 *Generation = DirtyGenerations.Find(Record.Id);
    if (!Generation) {
      continue;
    }

    UpsertsJsonArray.Add(MakeShareable(new FJsonValueObject(
      URedwoodCommonGameSubsystem::SerializeItemRecord(Record)
    )));
    SentUpserts.Add(TPair<FString, uint64>(Record.Id, *Generation));
  }

  if (UpsertsJsonArray.Num() != DirtyGenerations.Num()) {
    // A dirty item id that isn't in RecordsArray at all -- the game marked it dirty but never
    // wrote a matching record into the items array before the flush ran. Not fatal (the deletion,
    // if any, still goes out), but worth surfacing since that record is silently skipped this
    // flush. It stays dirty (not in SentUpserts) so a later flush, once a matching record exists,
    // retries it.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "FlushItemsForCharacterComponent: %d dirty item id(s) had no matching record in %s (skipped, left dirty)"
      ),
      DirtyGenerations.Num() - UpsertsJsonArray.Num(),
      *CharacterComponent->ItemsVariableName
    );
  }

  TArray<TSharedPtr<FJsonValue>> DeletesJsonArray;
  TArray<TPair<FString, uint64>> SentDeletes;
  for (const TPair<FString, uint64> &Deleted : DeletedGenerations) {
    DeletesJsonArray.Add(MakeShareable(new FJsonValueString(Deleted.Key)));
    SentDeletes.Add(Deleted);
  }

  if (UpsertsJsonArray.Num() == 0 && DeletesJsonArray.Num() == 0) {
    // Nothing actually sendable (e.g. every dirty id lacked a record). Release the flight slot we
    // just claimed via the ack path with no sent ids and the current committed seq, so the next
    // flush isn't wedged behind a slot that never sent anything. Settle synchronously: no emit
    // means no async callback will ever settle this attempt.
    CharacterComponent->CompleteItemFlush(
      TEXT(""), CharacterComponent->GetLastCommittedItemSeq(), {}, {}
    );
    Settle();
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetField(TEXT("id"), MakeShareable(new FJsonValueNull()));
  Payload->SetStringField(
    TEXT("characterId"),
    PlayerStateComponent ? PlayerStateComponent->RedwoodCharacter.Id
                         : CharacterComponent->RedwoodCharacterId
  );
  Payload->SetNumberField(TEXT("batchSeq"), static_cast<double>(BatchSeq));
  Payload->SetArrayField(TEXT("upserts"), UpsertsJsonArray);
  Payload->SetArrayField(TEXT("deletes"), DeletesJsonArray);

  // SentUpserts/SentDeletes above are already independent copies of the (id, generation) pairs
  // sent in this payload -- not references into CharacterComponent's live maps, which may change
  // again (a re-dirty bumping a generation, a new mark, etc.) before this ack arrives.
  TWeakObjectPtr<URedwoodCharacterComponent> WeakCharacterComponent = CharacterComponent;

  Sidecar->Emit(
    TEXT("realm:characters:items:flush"),
    Payload,
    [WeakCharacterComponent, SentUpserts, SentDeletes, OnAckSettled](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));
      // committedSeq rides both success and error (fence/replay) responses; TryGetNumberField
      // leaves the 0 default when absent (e.g. some error responses), which the error path treats
      // as "no fence ahead of us" and leaves NextBatchSeq alone -- and avoids a spurious missing-
      // field warning that GetNumberField would log.
      double CommittedSeqValue = 0.0;
      MessageStruct->TryGetNumberField(TEXT("committedSeq"), CommittedSeqValue);
      int64 CommittedSeq = static_cast<int64>(CommittedSeqValue);

      URedwoodCharacterComponent *CharacterComponent = WeakCharacterComponent.Get();
      if (!IsValid(CharacterComponent)) {
        // The player disconnected (or the pawn/PlayerState was destroyed) while this request was
        // in flight -- the single-flight slot lives on that now-dead component, so there is
        // nothing to release or clear. (Inherited exposure: the flush barrier that bounds a
        // detached pawn's lifetime keeps this from leaking a live component's slot.)
        if (!Error.IsEmpty()) {
          UE_LOG(
            LogRedwood, Error, TEXT("Failed to flush items (component gone): %s"), *Error
          );
        }
        // Still settle: the detached release barrier must not stall forever just because the
        // component died mid-flight -- the attempt HAS resolved.
        if (OnAckSettled) {
          OnAckSettled();
        }
        return;
      }

      if (!Error.IsEmpty()) {
        UE_LOG(LogRedwood, Error, TEXT("Failed to flush items: %s"), *Error);
      }

      // ALWAYS complete (success or error): CompleteItemFlush releases the single-flight slot and,
      // on success, clears only the generation-matched sent ids and advances the committed seq; on
      // error it leaves the dirt for the next flush (realigning NextBatchSeq only if the backend
      // reports a fence at/past the seq we sent).
      CharacterComponent->CompleteItemFlush(
        Error, CommittedSeq, SentUpserts, SentDeletes
      );

      // Settle AFTER CompleteItemFlush, on both success and error: the release barrier's rule is
      // "the attempt resolved", which is exactly here.
      if (OnAckSettled) {
        OnAckSettled();
      }
    }
  );
}

bool URedwoodServerGameSubsystem::AppendOfflineItemRows(
  TArray<TSharedPtr<FJsonValue>> &OutRows,
  URedwoodCharacterComponent *CharacterComponent
) {
  if (!CharacterComponent->bUseItems) {
    return false;
  }

  TArray<FRedwoodItemRecord> *RecordsArray =
    URedwoodCommonGameSubsystem::ResolveItemsRecordsArray(CharacterComponent);
  if (!RecordsArray) {
    // The game's ItemsVariableName property is missing or the wrong type -- a hard
    // misconfiguration that breaks the backend leg identically, already logged by
    // ResolveItemsRecordsArray. There is no set of records to write, and nothing better to
    // fall back to: SaveCharacterJsonToDisk rewrites the file wholesale, so the previous rows are
    // gone either way.
    return false;
  }

  // Honor pending deletions even though game code is expected to have already removed the record
  // from RecordsArray -- this leg writes the whole array verbatim, so a record left behind after
  // its deletion was marked would silently resurrect the item on the next load.
  const TMap<FString, uint64> &DeletedGenerations =
    CharacterComponent->GetPendingDeletedItemGenerations();

  TArray<FRedwoodItemRecord> RecordsToWrite;
  RecordsToWrite.Reserve(RecordsArray->Num());
  for (const FRedwoodItemRecord &Record : *RecordsArray) {
    if (!DeletedGenerations.Contains(Record.Id)) {
      RecordsToWrite.Add(Record);
    }
  }

  // APPEND, never assign: "items" is ONE field on the character object but every character
  // component owning items contributes rows to it, so the caller writes the field once from
  // the union of all of them. Setting the field per component would leave only the last component's
  // rows on disk and silently drop the rest.
  OutRows.Append(
    URedwoodCommonGameSubsystem::SerializeItemRecords(RecordsToWrite)
  );

  // Safe to clear outright, unlike the sidecar leg's generation-matched ack: the disk write is
  // synchronous and unconditionally rewrites the complete set, so no id needs to stay marked for
  // a later retry and no re-dirty can race an in-flight send. Without this, the dirty and
  // pending-deletion maps would grow for the whole session (ClearDirtyFlags deliberately leaves
  // item state alone) and a deleted id would suppress its own row forever.
  CharacterComponent->ClearItemsDirtyState();
  return true;
}

// One-shot migrate emit (Plan C's legacy-inventory backfill). Hands the backend a whole set of
// item rows to fold into the Item table; does NOT touch the component's dirty/flush state -- the
// rows are supplied whole by the caller, not drained from the dirty maps. See the header for the
// alreadyMigrated contract.
void URedwoodServerGameSubsystem::EmitItemsMigrate(
  URedwoodPlayerStateComponent *PlayerStateComponent,
  const TArray<FRedwoodItemRecord> &Items,
  TFunction<void(FString Error, bool bAlreadyMigrated)> OnComplete
) {
  // Null-PSC guard mirroring FlushItemsForCharacterComponent's tolerance -- except migrate has no
  // CharacterComponent to fall back to for the characterId (the rows are handed in whole, not
  // drained from a component), so a null PSC is unrecoverable: early-return with a warning rather
  // than dereferencing it below.
  if (!PlayerStateComponent) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("EmitItemsMigrate: null PlayerStateComponent; cannot resolve characterId")
    );
    if (OnComplete) {
      OnComplete(TEXT("Null PlayerStateComponent"), false);
    }
    return;
  }

  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood, Error, TEXT("Sidecar is not connected; cannot migrate items")
    );
    if (OnComplete) {
      OnComplete(TEXT("Sidecar is not connected"), false);
    }
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(
    TEXT("characterId"), PlayerStateComponent->RedwoodCharacter.Id
  );
  Payload->SetArrayField(
    TEXT("items"), URedwoodCommonGameSubsystem::SerializeItemRecords(Items)
  );

  Sidecar->Emit(
    TEXT("realm:characters:items:migrate"),
    Payload,
    [OnComplete](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));
      // Absent -> false: an error response may omit alreadyMigrated entirely.
      bool bAlreadyMigrated = false;
      MessageStruct->TryGetBoolField(TEXT("alreadyMigrated"), bAlreadyMigrated);
      if (!Error.IsEmpty()) {
        UE_LOG(LogRedwood, Error, TEXT("Failed to migrate items: %s"), *Error);
      }
      if (OnComplete) {
        OnComplete(Error, bAlreadyMigrated);
      }
    }
  );
}

// One-shot trade emit: atomically re-parents the named root items from one character to another on
// the backend. rootPlacements carries only each root row's (id, domain, slot); the backend moves
// each root and its nested children in one transaction and bumps BOTH characters' InventorySeq, so
// each side's next FlushItemsForCharacterComponent takes the seq-resync path (see the header).
void URedwoodServerGameSubsystem::EmitItemsTrade(
  const FString &FromCharacterId,
  const FString &ToCharacterId,
  const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
  TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)>
    OnComplete
) {
  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood, Error, TEXT("Sidecar is not connected; cannot trade items")
    );
    if (OnComplete) {
      OnComplete(TEXT("Sidecar is not connected"), -1, -1);
    }
    return;
  }

  TArray<TSharedPtr<FJsonValue>> RootPlacementsJsonArray;
  RootPlacementsJsonArray.Reserve(RootPlacements.Num());
  for (const FRedwoodTradeRootPlacement &Placement : RootPlacements) {
    TSharedPtr<FJsonObject> PlacementObject = MakeShareable(new FJsonObject);
    PlacementObject->SetStringField(TEXT("id"), Placement.Id);
    PlacementObject->SetStringField(TEXT("domain"), Placement.Domain);
    PlacementObject->SetNumberField(TEXT("slot"), Placement.Slot);
    RootPlacementsJsonArray.Add(
      MakeShareable(new FJsonValueObject(PlacementObject))
    );
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("fromCharacterId"), FromCharacterId);
  Payload->SetStringField(TEXT("toCharacterId"), ToCharacterId);
  Payload->SetArrayField(TEXT("rootPlacements"), RootPlacementsJsonArray);

  Sidecar->Emit(
    TEXT("realm:characters:items:trade"),
    Payload,
    [OnComplete](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));
      if (!Error.IsEmpty()) {
        UE_LOG(LogRedwood, Error, TEXT("Failed to trade items: %s"), *Error);
      }
      // FORK(hollowed-oath): post-trade InventorySeq fences (see the header doc-comment on
      // EmitItemsTrade for the full delta-loss contract). -1 default covers error acks that omit
      // the fields entirely, matching the sentinel the backend itself sends on error paths. Routed
      // through a double intermediate the same way committedSeq is above -- JSON numbers land as
      // doubles, and TryGetNumberField has no int64 overload here.
      double FromCommittedSeqValue = -1.0;
      double ToCommittedSeqValue = -1.0;
      MessageStruct->TryGetNumberField(
        TEXT("fromCommittedSeq"), FromCommittedSeqValue
      );
      MessageStruct->TryGetNumberField(
        TEXT("toCommittedSeq"), ToCommittedSeqValue
      );
      int64 FromCommittedSeq = static_cast<int64>(FromCommittedSeqValue);
      int64 ToCommittedSeq = static_cast<int64>(ToCommittedSeqValue);
      if (OnComplete) {
        OnComplete(Error, FromCommittedSeq, ToCommittedSeq);
      }
    }
  );
}
// FORK(hollowed-oath) END

void URedwoodServerGameSubsystem::InitialDataLoad(FRedwoodDelegate OnComplete) {
  InitialDataLoadCompleteDelegate = OnComplete;

  if (URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
    if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Sidecar is not connected; cannot load initial data")
      );
      return;
    }

    TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
    Sidecar->Emit(
      TEXT("realm:servers:session:persistence:initial-load"),
      Payload,
      [this](auto Response) {
        TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
        FString Error = MessageStruct->GetStringField(TEXT("error"));

        if (!Error.IsEmpty()) {
          UE_LOG(
            LogRedwood, Error, TEXT("Failed to load initial data: %s"), *Error
          );
        } else {
          UE_LOG(LogRedwood, Log, TEXT("Loaded initial data"));
          PostInitialDataLoad(MessageStruct);
        }
      }
    );

  } else {
    // load from disk
    FString SavePath =
      FPaths::ProjectSavedDir() / TEXT("Persistence") / TEXT("Maps");
    FPaths::NormalizeDirectoryName(SavePath);

    if (!FPaths::DirectoryExists(SavePath)) {
      IFileManager::Get().MakeDirectory(*SavePath, true);
    }

    UWorld *World = GetWorld();
    if (!IsValid(World)) {
      UE_LOG(
        LogRedwood, Error, TEXT("Can't InitialDataLoad: World is not valid")
      );
      return;
    }

    FString MapName = World->GetMapName();

    FString MapSavePath = SavePath / MapName + TEXT(".json");

    TSharedPtr<FJsonObject> MapJsonObject;
    if (FPaths::FileExists(MapSavePath)) {
      FString JsonString;
      FFileHelper::LoadFileToString(JsonString, *MapSavePath);

      TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(JsonString);
      if (!FJsonSerializer::Deserialize(Reader, MapJsonObject)) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Failed to deserialize JSON for map %s"),
          *MapName
        );
      }
    } else {
      UE_LOG(
        LogRedwood,
        Log,
        TEXT(
          "URedwoodServerGameSubsystem::InitialDataLoad: No saved data for map %s"
        ),
        *MapName
      );
      MapJsonObject = MakeShareable(new FJsonObject);
    }

    PostInitialDataLoad(MapJsonObject);
  }
}

void URedwoodServerGameSubsystem::PostInitialDataLoad(
  TSharedPtr<FJsonObject> InitialLoadJsonObject
) {
  FRedwoodInitialLoadData InitialLoad =
    URedwoodCommonGameSubsystem::ParseInitialLoadData(InitialLoadJsonObject);

  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(
      LogRedwood, Error, TEXT("Can't PostInitialDataLoad: World is not valid")
    );
    return;
  }

  AGameStateBase *GameState = World->GetGameState();
  if (!IsValid(GameState)) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Can't PostInitialDataLoad: GameState is not valid")
    );
    return;
  }

  TArray<URedwoodSyncComponent *> GameStateSyncComponents;
  GameState->GetComponents<URedwoodSyncComponent>(GameStateSyncComponents);
  for (URedwoodSyncComponent *GameStateSync : GameStateSyncComponents) {
    if (GameStateSync->RedwoodId == TEXT("proxy") || (GameStateSyncComponents.Num() == 1 && GameStateSync->RedwoodId.IsEmpty())) {
      UpdateSyncItemData(GameStateSync, InitialLoad.Data);
    } else if (GameStateSync->RedwoodId == TEXT("zone")) {
      UpdateSyncItemData(GameStateSync, InitialLoad.ZoneData);
    }
  }

  for (FRedwoodSyncItem &Item : InitialLoad.Items) {
    UpdateSyncItem(Item);
  }

  bInitialDataLoaded = true;
  InitialDataLoadCompleteDelegate.ExecuteIfBound();
  SendNewSyncForPersistentItemsToSidecar();
}

void URedwoodServerGameSubsystem::UpdateSyncItem(FRedwoodSyncItem &Item) {
  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(LogRedwood, Error, TEXT("Can't UpdateSyncItem: World is not valid"));
    return;
  }

  URedwoodSyncComponent *SyncItemComponent = nullptr;
  bool bGameStateComponent =
    Item.State.Id == TEXT("proxy") || Item.State.Id == TEXT("zone");

  if (bGameStateComponent) {
    AGameStateBase *GameState = World->GetGameState();
    if (!IsValid(GameState)) {
      UE_LOG(
        LogRedwood, Error, TEXT("Can't UpdateSyncItem: GameState is not valid")
      );
      return;
    }

    TArray<URedwoodSyncComponent *> GameStateSyncComponents;
    GameState->GetComponents<URedwoodSyncComponent>(GameStateSyncComponents);
    for (URedwoodSyncComponent *GameStateSync : GameStateSyncComponents) {
      if (GameStateSync->RedwoodId == Item.State.Id) {
        SyncItemComponent = GameStateSync;
        break;
      }
    }

    if (SyncItemComponent == nullptr) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "Can't UpdateSyncItem: GameState has no RedwoodSyncComponent with id %s"
        ),
        *Item.State.Id
      );
      return;
    }
  } else {
    if (TWeakObjectPtr<URedwoodSyncComponent> *WeakPtr = SyncItemComponentsById.Find(Item.State.Id)) {
      SyncItemComponent = WeakPtr->Get();
      if (SyncItemComponent == nullptr) {
        // entry was GC'd; drop it and respawn below
        SyncItemComponentsById.Remove(Item.State.Id);
      }
    }

    if (SyncItemComponent == nullptr) {
      // spawn it
      URedwoodSyncItemAsset *ItemType = nullptr;
      AActor *Actor = nullptr;
      if (TWeakObjectPtr<URedwoodSyncItemAsset> *WeakItemType =
      SyncItemTypesByTypeId.Find(Item.State.TypeId)) {
        ItemType = WeakItemType->Get();
      }

      if (!IsValid(ItemType)) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT(
            "Can't spawn SyncItemComponent of type %s because it's not registered"
          ),
          *Item.State.TypeId
        );
        return;
      }

      TSoftClassPtr<AActor> ActorClass = ItemType->ActorClass;
      if (ActorClass.IsNull()) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT(
            "Can't spawn SyncItemComponent of type %s because it has no ActorClass"
          ),
          *Item.State.TypeId
        );
        return;
      }

      Actor = World->SpawnActor<AActor>(ActorClass.Get());

      if (Actor == nullptr) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Failed to spawn SyncItemComponent of type %s"),
          *Item.State.TypeId
        );
        return;
      }

      SyncItemComponent = Actor->GetComponentByClass<URedwoodSyncComponent>();

      if (SyncItemComponent == nullptr) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT(
            "Spawned actor for SyncItemComponent of type %s, but the actor has no RedwoodSyncComponent"
          ),
          *Item.State.TypeId
        );
        return;
      }

      SyncItemComponent->RedwoodId = Item.State.Id;
      SyncItemComponent->SkipInitialSave();

      SyncItemComponentsById.Add(Item.State.Id, SyncItemComponent);
    }
  }

  if (!bGameStateComponent) {
    UpdateSyncItemState(SyncItemComponent, Item.State);
  }

  UpdateSyncItemData(SyncItemComponent, Item.Data);

  if (!bGameStateComponent) {
    UpdateSyncItemMovement(SyncItemComponent, Item.Movement);
  }
}

void URedwoodServerGameSubsystem::UpdateSyncItemState(
  URedwoodSyncComponent *SyncItemComponent, FRedwoodSyncItemState &ItemState
) {
  if (IsValid(SyncItemComponent)) {
    SyncItemComponent->ZoneName = ItemState.ZoneName;
  }
}

void URedwoodServerGameSubsystem::UpdateSyncItemMovement(
  URedwoodSyncComponent *SyncItemComponent,
  FRedwoodSyncItemMovement &ItemMovement
) {
  if (IsValid(SyncItemComponent)) {
    AActor *Actor = SyncItemComponent->GetOwner();
    USceneComponent *ActorRootComponent = Actor->GetRootComponent();

    if (ActorRootComponent == nullptr) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT(
          "SyncItemComponent %s has no root component; can't update transform"
        ),
        *SyncItemComponent->RedwoodId
      );
      return;
    }

    ActorRootComponent->SetWorldTransform(ItemMovement.Transform);
  }
}

void URedwoodServerGameSubsystem::UpdateSyncItemData(
  URedwoodSyncComponent *SyncItemComponent, USIOJsonObject *InData
) {
  if (IsValid(SyncItemComponent) && IsValid(InData)) {
    AActor *Actor = SyncItemComponent->GetOwner();
    bool bErrored = false;
    bool bDirty = URedwoodCommonGameSubsystem::DeserializeBackendData(
      SyncItemComponent->bStoreDataInActor ? (UObject *)Actor
                                           : (UObject *)SyncItemComponent,
      InData,
      SyncItemComponent->DataVariableName,
      SyncItemComponent->LatestDataSchemaVersion,
      bErrored
    );

    if (bDirty) {
      SyncItemComponent->MarkDataDirty();
    }

    SyncItemComponent->DataUpdated.Broadcast();
  }
}

void URedwoodServerGameSubsystem::FlushZoneData() {
  TSharedPtr<FJsonObject> ZoneData = MakeShareable(new FJsonObject);

  bool bZoneDataDirty = false;

  UWorld *World = GetWorld();
  if (!IsValid(World)) {
    UE_LOG(LogRedwood, Error, TEXT("Can't FlushZoneData: World is not valid"));
    return;
  }

  AGameStateBase *GameState = World->GetGameState();
  if (!IsValid(GameState)) {
    UE_LOG(
      LogRedwood, Error, TEXT("Can't FlushZoneData: GameState is not valid")
    );
    return;
  }

  TArray<URedwoodSyncComponent *> GameStateSyncComponents;
  GameState->GetComponents<URedwoodSyncComponent>(GameStateSyncComponents);
  for (URedwoodSyncComponent *GameStateSync : GameStateSyncComponents) {
    if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld()) ||
        GameStateSync->IsDataDirty(true) ||
        GameStateSync->ShouldDoInitialSave()) {
      // always add the data if we're not using the backend

      if (GameStateSync->IsDataDirty(true)) {
        bZoneDataDirty = true;
        GameStateSync->ClearDirtyFlags(true);
      }

      if (GameStateSync->ShouldDoInitialSave() && GameStateSync->bPersistChanges) {
        bZoneDataDirty = true;
        GameStateSync->SkipInitialSave();
      }

      USIOJsonObject *DataObject =
        URedwoodCommonGameSubsystem::SerializeBackendData(
          GameStateSync->bStoreDataInActor ? (UObject *)GameState
                                           : (UObject *)GameStateSync,
          GameStateSync->DataVariableName
        );

      if (GameStateSync->RedwoodId == TEXT("proxy") || (GameStateSyncComponents.Num() == 1 && GameStateSync->RedwoodId.IsEmpty())) {
        ZoneData->SetObjectField(TEXT("data"), DataObject->GetRootObject());
      } else if (GameStateSync->RedwoodId == TEXT("zone")) {
        ZoneData->SetObjectField(TEXT("zoneData"), DataObject->GetRootObject());
      }
    }
  }

  // get data from SyncItemComponentsById
  TArray<TSharedPtr<FJsonValue>> PersistentItemsArray;
  for (auto It = SyncItemComponentsById.CreateIterator(); It; ++It) {
    TWeakObjectPtr<URedwoodSyncComponent> &WeakComp = It.Value();

    URedwoodSyncComponent *SyncItemComponent = WeakComp.Get();

    if (SyncItemComponent == nullptr) {
      It.RemoveCurrent(); // remove dead entry safely
      continue;
    }

    if (SyncItemComponent->RedwoodId == TEXT("proxy") || SyncItemComponent->RedwoodId == TEXT("zone")) {
      // don't save the proxy/world data here
      continue;
    }

    bool bUpdateItem = (SyncItemComponent->IsMovementDirty(true) ||
                        SyncItemComponent->IsDataDirty(true) ||
                        SyncItemComponent->ShouldDoInitialSave()) &&
      SyncItemComponent->bPersistChanges;

    if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld()) || bUpdateItem) {
      // always add the data if we're not using the backend

      if (bUpdateItem) {
        bZoneDataDirty = true;
      }

      TSharedPtr<FJsonObject> ItemObject = MakeShareable(new FJsonObject());

      ItemObject->SetStringField(TEXT("id"), SyncItemComponent->RedwoodId);
      ItemObject->SetStringField(
        TEXT("typeId"), SyncItemComponent->RedwoodTypeId
      );

      // This flag should be redundant as we already checked the bools above
      // but we'll keep it just for now.
      bool bItemShouldBeSaved = false;

      if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld()) ||
          SyncItemComponent->IsMovementDirty(true) ||
          SyncItemComponent->ShouldDoInitialSave()) {
        bItemShouldBeSaved = true;

        FTransform Transform = SyncItemComponent->GetOwner()->GetTransform();
        FVector Location = Transform.GetLocation();
        FVector Rotation = Transform.GetRotation().Euler();
        FVector Scale = Transform.GetScale3D();

        TSharedPtr<FJsonObject> TransformObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> LocationObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> RotationObject =
          MakeShareable(new FJsonObject());
        TSharedPtr<FJsonObject> ScaleObject = MakeShareable(new FJsonObject());
        LocationObject->SetNumberField(TEXT("x"), Location.X);
        LocationObject->SetNumberField(TEXT("y"), Location.Y);
        LocationObject->SetNumberField(TEXT("z"), Location.Z);
        RotationObject->SetNumberField(TEXT("x"), Rotation.X);
        RotationObject->SetNumberField(TEXT("y"), Rotation.Y);
        RotationObject->SetNumberField(TEXT("z"), Rotation.Z);
        ScaleObject->SetNumberField(TEXT("x"), Scale.X);
        ScaleObject->SetNumberField(TEXT("y"), Scale.Y);
        ScaleObject->SetNumberField(TEXT("z"), Scale.Z);
        TransformObject->SetObjectField(TEXT("location"), LocationObject);
        TransformObject->SetObjectField(TEXT("rotation"), RotationObject);
        TransformObject->SetObjectField(TEXT("scale"), ScaleObject);
        ItemObject->SetObjectField(TEXT("transform"), TransformObject);
      }

      if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld()) ||
          SyncItemComponent->IsDataDirty(true) ||
          SyncItemComponent->ShouldDoInitialSave()) {
        bItemShouldBeSaved = true;

        USIOJsonObject *DataObject =
          URedwoodCommonGameSubsystem::SerializeBackendData(
            SyncItemComponent->bStoreDataInActor
              ? (UObject *)SyncItemComponent->GetOwner()
              : (UObject *)SyncItemComponent,
            SyncItemComponent->DataVariableName
          );
        ItemObject->SetObjectField(TEXT("data"), DataObject->GetRootObject());
      }

      if (SyncItemComponent->ShouldDoInitialSave()) {
        SyncItemComponent->SkipInitialSave();
      }

      SyncItemComponent->ClearDirtyFlags(true);

      if (bItemShouldBeSaved) {
        TSharedPtr<FJsonValueObject> Value =
          MakeShareable(new FJsonValueObject(ItemObject));
        PersistentItemsArray.Add(Value);
      }
    }
  }

  ZoneData->SetArrayField(TEXT("persistentItems"), PersistentItemsArray);

  if (bZoneDataDirty) {
    if (URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
      if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Sidecar is not connected; cannot flush zone data")
        );
        return;
      }

      Sidecar->Emit(
        TEXT("realm:servers:session:persistence:update-zone"), ZoneData
      );
    } else {
      // save to disk

      UE_LOG(LogRedwood, Log, TEXT("Saving zone data to disk"));

      FString SavePath =
        FPaths::ProjectSavedDir() / TEXT("Persistence") / TEXT("Maps");
      FPaths::NormalizeDirectoryName(SavePath);

      if (!FPaths::DirectoryExists(SavePath)) {
        IFileManager::Get().MakeDirectory(*SavePath, true);
      }

      FString MapName = World->GetMapName();
      FString MapSavePath = SavePath / MapName + TEXT(".json");

      FString JsonString;
      TSharedRef<TJsonWriter<>> Writer =
        TJsonWriterFactory<>::Create(&JsonString);
      FJsonSerializer::Serialize(ZoneData.ToSharedRef(), Writer);

      FFileHelper::SaveStringToFile(JsonString, *MapSavePath);
    }
  }
}

void URedwoodServerGameSubsystem::RegisterSyncComponent(
  URedwoodSyncComponent *InComponent, bool bDelayNewSync
) {
  if (InComponent == nullptr) {
    UE_LOG(
      LogRedwood, Error, TEXT("Can't register null URedwoodSyncComponent")
    );
    return;
  }

  if (SyncItemComponentsById.FindRef(InComponent->RedwoodId) != nullptr) {
    return;
  }

  // this should be a newly spawned item that didn't get synced in externally,
  // register it as a new sync item

  if (InComponent->RedwoodId.IsEmpty()) {
    InComponent->RedwoodId = FGuid::NewGuid().ToString();
  }

  InComponent->ZoneName = ZoneName;

  SyncItemComponentsById.Add(InComponent->RedwoodId, InComponent);

  UClass *ActorClass = InComponent->GetOwner()->GetClass();
  for (auto It = SyncItemTypesByTypeId.CreateIterator(); It; ++It) {
    TWeakObjectPtr<URedwoodSyncItemAsset> WeakSyncItemType = It.Value();

    if (URedwoodSyncItemAsset *SyncItemType = WeakSyncItemType.Get()) {
      if (SyncItemType->ActorClass.Get() == ActorClass) {
        InComponent->RedwoodTypeId = It.Key();
        break;
      }
    } else {
      It.RemoveCurrent(); // remove dead entry safely
    }
  }

  if (InComponent->RedwoodTypeId.IsEmpty()) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT(
        "Failed to find a SyncItemAsset with an ActorClass of %s for SyncItemComponent %s"
      ),
      *ActorClass->GetName(),
      *InComponent->RedwoodId
    );
  }

  if (!bDelayNewSync || bInitialDataLoaded) {
    SendNewSyncItemToSidecar(InComponent);
  } else {
    DelayedNewSyncItems.Add(InComponent);
  }
}

void URedwoodServerGameSubsystem::SendNewSyncItemToSidecar(
  URedwoodSyncComponent *InComponent
) {
  if (URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld()) &&
      Sidecar.IsValid() && Sidecar->bIsConnected) {
    TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

    TSharedPtr<FJsonObject> StateObject = MakeShareable(new FJsonObject);
    StateObject->SetStringField(TEXT("id"), InComponent->RedwoodId);
    StateObject->SetStringField(TEXT("typeId"), InComponent->RedwoodTypeId);
    StateObject->SetBoolField(TEXT("destroyed"), false);
    StateObject->SetStringField(TEXT("zoneName"), InComponent->ZoneName);

    Payload->SetObjectField(TEXT("state"), StateObject);

    TSharedPtr<FJsonObject> MovementObject = MakeShareable(new FJsonObject);

    FTransform Transform = InComponent->GetOwner()->GetTransform();
    FVector Location = Transform.GetLocation();
    FVector Rotation = Transform.GetRotation().Euler();
    FVector Scale = Transform.GetScale3D();

    TSharedPtr<FJsonObject> TransformObject = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> LocationObject = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> RotationObject = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> ScaleObject = MakeShareable(new FJsonObject());
    LocationObject->SetNumberField(TEXT("x"), Location.X);
    LocationObject->SetNumberField(TEXT("y"), Location.Y);
    LocationObject->SetNumberField(TEXT("z"), Location.Z);
    RotationObject->SetNumberField(TEXT("x"), Rotation.X);
    RotationObject->SetNumberField(TEXT("y"), Rotation.Y);
    RotationObject->SetNumberField(TEXT("z"), Rotation.Z);
    ScaleObject->SetNumberField(TEXT("x"), Scale.X);
    ScaleObject->SetNumberField(TEXT("y"), Scale.Y);
    ScaleObject->SetNumberField(TEXT("z"), Scale.Z);
    TransformObject->SetObjectField(TEXT("location"), LocationObject);
    TransformObject->SetObjectField(TEXT("rotation"), RotationObject);
    TransformObject->SetObjectField(TEXT("scale"), ScaleObject);
    MovementObject->SetObjectField(TEXT("transform"), TransformObject);

    Payload->SetObjectField(TEXT("movement"), MovementObject);

    USIOJsonObject *DataObject =
      URedwoodCommonGameSubsystem::SerializeBackendData(
        InComponent->bStoreDataInActor ? (UObject *)InComponent->GetOwner()
                                       : (UObject *)InComponent,
        InComponent->DataVariableName
      );
    Payload->SetObjectField(TEXT("data"), DataObject->GetRootObject());

    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Sending new sync item %s (type %s) to backend"),
      *InComponent->RedwoodId,
      *InComponent->RedwoodTypeId
    );

    Sidecar->Emit(TEXT("realm:servers:session:sync:new"), Payload);
  }
}

void URedwoodServerGameSubsystem::SendNewSyncForPersistentItemsToSidecar() {
  for (auto It = DelayedNewSyncItems.CreateIterator(); It; ++It) {
    TWeakObjectPtr<URedwoodSyncComponent> WeakSyncItemComponent = *It;

    if (URedwoodSyncComponent *SyncItemComponent = WeakSyncItemComponent.Get()) {
      SendNewSyncItemToSidecar(SyncItemComponent);
    } else {
      It.RemoveCurrent(); // remove dead entry safely
    }
  }
}

void URedwoodServerGameSubsystem::PutBlob(
  const FString &Key,
  const TArray<uint8> &Value,
  FRedwoodErrorOutputDelegate OnComplete
) {
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  Payload->SetStringField(TEXT("id"), "game-server");
  Payload->SetStringField(TEXT("key"), Key);

  TSharedPtr<FJsonValueBinary> BinaryValue =
    MakeShareable(new FJsonValueBinary(Value));
  Payload->SetField(TEXT("blob"), BinaryValue);

  Sidecar->Emit(TEXT("realm:blobs:put"), Payload, [OnComplete](auto Response) {
    TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
    FString Error = MessageStruct->GetStringField(TEXT("error"));

    OnComplete.ExecuteIfBound(Error);
  });
}

void URedwoodServerGameSubsystem::GetBlob(
  const FString &Key, FRedwoodGetBlobOutputDelegate OnComplete
) {
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);

  Payload->SetStringField(TEXT("id"), "game-server");
  Payload->SetStringField(TEXT("key"), Key);

  Sidecar
    ->Emit(TEXT("realm:blobs:get"), Payload, [this, OnComplete](auto Response) {
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));

      if (!Error.IsEmpty()) {
        FRedwoodGetBlobOutput Output;
        Output.Error = Error;
        OnComplete.ExecuteIfBound(Output);
        return;
      }

      FRedwoodGetBlobOutput Output;

      TSharedPtr<FJsonValue> Value = MessageStruct->TryGetField(TEXT("blob"));
      if (Value.IsValid() && FJsonValueBinary::IsBinary(Value)) {
        Output.Blob = FJsonValueBinary::AsBinary(Value);
      } else {
        Output.Error = TEXT("Failed to parse blob");
      }

      OnComplete.ExecuteIfBound(Output);
    });
}

void URedwoodServerGameSubsystem::PutSaveGame(
  const FString &Key, USaveGame *Value, FRedwoodErrorOutputDelegate OnComplete
) {
  TSharedRef<TArray<uint8>> ObjectBytes(new TArray<uint8>());

  if (UGameplayStatics::SaveGameToMemory(Value, *ObjectBytes) && (ObjectBytes->Num() > 0)) {
    PutBlob(Key, *ObjectBytes, OnComplete);
  } else {
    OnComplete.ExecuteIfBound(TEXT("Failed to serialize SaveGame"));
  }
}

void URedwoodServerGameSubsystem::GetSaveGame(
  const FString &Key, FRedwoodGetSaveGameOutputDelegate OnComplete
) {
  GetBlob(
    Key,
    FRedwoodGetBlobOutputDelegate::CreateLambda(
      [this, OnComplete](FRedwoodGetBlobOutput Response) {
        FRedwoodGetSaveGameOutput Output;

        if (!Response.Error.IsEmpty()) {
          Output.Error = Response.Error;
          Output.SaveGame = nullptr;
          OnComplete.ExecuteIfBound(Output);
          return;
        }

        if (Response.Blob.Num() > 0) {
          Output.SaveGame = UGameplayStatics::LoadGameFromMemory(Response.Blob);
        }

        OnComplete.ExecuteIfBound(Output);
      }
    )
  );
}

void URedwoodServerGameSubsystem::GetPartyById(
  const FString &PartyId, FRedwoodGetPartyOutputDelegate OnOutput
) {
  GetParty(PartyId, TEXT(""), OnOutput);
}

void URedwoodServerGameSubsystem::GetPartyByPlayerId(
  const FString &PlayerId, FRedwoodGetPartyOutputDelegate OnOutput
) {
  GetParty(TEXT(""), PlayerId, OnOutput);
}

void URedwoodServerGameSubsystem::GetParty(
  const FString &PartyId,
  const FString &PlayerId,
  FRedwoodGetPartyOutputDelegate OnOutput
) {
  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    FRedwoodGetPartyOutput Output;
    Output.Error = TEXT("Sidecar is not connected");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), TEXT("game-server"));

  TSharedPtr<FJsonValue> NullValue = MakeShareable(new FJsonValueNull());

  if (PartyId.IsEmpty()) {
    Payload->SetField(TEXT("partyId"), NullValue);
  } else {
    Payload->SetStringField(TEXT("partyId"), PartyId);
  }

  if (PlayerId.IsEmpty()) {
    Payload->SetField(TEXT("playerId"), NullValue);
  } else {
    Payload->SetStringField(TEXT("playerId"), PlayerId);
  }

  Sidecar->Emit(
    TEXT("realm:parties:get:backend"),
    Payload,
    [OnOutput](auto Response) {
      TSharedPtr<FJsonObject> MessageObject = Response[0]->AsObject();

      FRedwoodGetPartyOutput Output;
      Output.Error = MessageObject->GetStringField(TEXT("error"));

      const TSharedPtr<FJsonObject> *PartyObj;
      if (MessageObject->TryGetObjectField(TEXT("party"), PartyObj)) {
        Output.Party = URedwoodCommonGameSubsystem::ParseParty(*PartyObj);
      }

      OnOutput.ExecuteIfBound(Output);
    }
  );
}

FRedwoodParty URedwoodServerGameSubsystem::GetTrackedPartyById(
  const FString &InPartyId
) const {
  if (const FRedwoodParty *Party = TrackedParties.Find(InPartyId)) {
    return *Party;
  }

  return FRedwoodParty();
}

FRedwoodParty URedwoodServerGameSubsystem::GetTrackedPartyByPlayerId(
  const FString &InPlayerId
) const {
  if (InPlayerId.IsEmpty()) {
    return FRedwoodParty();
  }

  for (const auto &Pair : TrackedParties) {
    for (const FRedwoodPartyMember &Member : Pair.Value.Members) {
      if (Member.PlayerId == InPlayerId) {
        return Pair.Value;
      }
    }
  }

  return FRedwoodParty();
}

bool URedwoodServerGameSubsystem::DoesServerHostPartyMember(
  const FRedwoodParty &Party
) const {
  if (!Party.bValid || Party.Members.Num() == 0) {
    return false;
  }

  UWorld *World = GetWorld();
  AGameStateBase *GameState = IsValid(World) ? World->GetGameState() : nullptr;

  if (!IsValid(GameState)) {
    return false;
  }

  TSet<FString> MemberPlayerIds;
  for (const FRedwoodPartyMember &Member : Party.Members) {
    MemberPlayerIds.Add(Member.PlayerId);
  }

  for (APlayerState *PlayerState : GameState->PlayerArray) {
    if (!IsValid(PlayerState)) {
      continue;
    }

    URedwoodPlayerStateComponent *PlayerStateComponent =
      PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();

    if (
      IsValid(PlayerStateComponent) &&
      MemberPlayerIds.Contains(PlayerStateComponent->RedwoodPlayer.Id)
    ) {
      return true;
    }
  }

  return false;
}

void URedwoodServerGameSubsystem::UpdatePlayerStateComponentPartyIds() {
  UWorld *World = GetWorld();
  AGameStateBase *GameState = IsValid(World) ? World->GetGameState() : nullptr;

  if (!IsValid(GameState)) {
    return;
  }

  TMap<FString, FString> PartyIdsByPlayerId;
  for (const auto &Pair : TrackedParties) {
    for (const FRedwoodPartyMember &Member : Pair.Value.Members) {
      PartyIdsByPlayerId.Add(Member.PlayerId, Pair.Key);
    }
  }

  for (APlayerState *PlayerState : GameState->PlayerArray) {
    if (!IsValid(PlayerState)) {
      continue;
    }

    URedwoodPlayerStateComponent *PlayerStateComponent =
      PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();

    if (!IsValid(PlayerStateComponent)) {
      continue;
    }

    const FString &PlayerId = PlayerStateComponent->RedwoodPlayer.Id;

    if (PlayerId.IsEmpty()) {
      // The player hasn't finished authenticating yet; their PartyId
      // gets synced once their RedwoodPlayer data is set.
      continue;
    }

    if (const FString *PartyId = PartyIdsByPlayerId.Find(PlayerId)) {
      PlayerStateComponent->SetPartyId(*PartyId);
    } else {
      PlayerStateComponent->SetPartyId(TEXT(""));
    }
  }
}

void URedwoodServerGameSubsystem::RequestEngineExit(bool bForce) {
  FGenericPlatformMisc::RequestExit(bForce);
}
