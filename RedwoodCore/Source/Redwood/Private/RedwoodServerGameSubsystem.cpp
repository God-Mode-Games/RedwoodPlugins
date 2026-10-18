// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodServerGameSubsystem.h"
#include "RedwoodAccountDocuments.h" // FORK(hollowed-oath)
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
// item flush / trade / offline helpers below.
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
  // FORK(hollowed-oath): a held flush attempt must not outlive the server. Its settle releases a
  // detached pawn's write binding, and its timer would fire on a dead game instance. The give-up
  // logs what stays unsaved. Keys first: a settle can change the map.
  // A trade that never settled gives up first: its held flushes then resume into the freeze.
  TArray<FString> TradeIds;
  PendingItemTrades.GetKeys(TradeIds);
  for (const FString &TradeId : TradeIds) {
    GiveUpItemsTrade(TradeId, TEXT("the server is shutting down"));
  }
  TArray<FString> ReloadingCharacterIds;
  CurrencyReloads.GetKeys(ReloadingCharacterIds);
  for (const FString &CharacterId : ReloadingCharacterIds) {
    GiveUpCurrencyReload(CharacterId, TEXT("the server is shutting down"));
  }

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

void URedwoodServerGameSubsystem::BindSidecarConnectedCallback() {
  Sidecar->OnConnectedCallback =
    [this](const FString &InSocketId, const FString &InSessionId) {
      HandleSidecarConnected();
    };
}

// FORK(hollowed-oath): the sidecar's connect event, which fires on every reconnect too.
void URedwoodServerGameSubsystem::HandleSidecarConnected() {
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
    // FORK(hollowed-oath): currency reloads and trade resends that waited for the sidecar go out
    // now.
    ResumeCurrencyReloadsAfterSidecarConnect();
    ResumeItemsTradesAfterSidecarConnect();
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

  // FORK(hollowed-oath): the realm tells the game server when a transfer it
  // had accepted then failed. Upstream has no such event. The rules live in
  // HandleTransferFailedEvent.
  Sidecar->OnEvent(
    TransferFailedEventName,
    [this](const FString &Event, const TSharedPtr<FJsonValue> &Message) {
      const TSharedPtr<FJsonObject> *Object;

      if (Message->TryGetObject(Object) && Object) {
        HandleTransferFailedEvent(*Object);
      }
    }
  );

  // FORK(hollowed-oath): the connect callback lives in BindSidecarConnectedCallback, so a test can
  // install the real one on its own socket.
  BindSidecarConnectedCallback();

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
    // FORK(hollowed-oath): InitTransferring ran above, so this failed path
    // must roll the flag back or the player stays latched as transferring
    // for the rest of the session. See AbortTransferring's rationale block.
    if (PlayerStateComponent) {
      PlayerStateComponent->AbortTransferring(
        SidecarDownError, SidecarDownReason
      );
    }
    return;
  }

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Traveling player %s to zone %s"),
    *PlayerId,
    *InZoneName
  );

  // FORK(hollowed-oath): the wait for the answer, and the emit itself.
  EmitTransferZoneRequest(PlayerStateComponent, PlayerController, Payload);
}

// FORK(hollowed-oath): the shared tail of both TravelPlayerToZone* functions.
// Upstream wrote this emit out in each of them and handled the answer inline;
// the answer rules now live once in HandleTransferZoneResponse, and the wait
// for an answer that never comes starts here. One function, so each upstream
// travel function keeps one fork line and the fork footprint in upstream code
// stays as small as it can be.
//
// A weak pointer for the player controller, which can go away before the
// sidecar answers; the subsystem stays a raw "this", as upstream has it,
// because it lives as long as the game instance. Upstream read Response[0]
// without a test; an answer with no value at all is out of range there, and
// the handler drops an empty answer anyway.
//
// The answer carries the name of the transfer that asked for it.
// InitTransferring ran before this, so a live transfer never has generation
// 0. A player with no component emits -1, which no component ever holds, so
// such an answer can match nothing later.
void URedwoodServerGameSubsystem::EmitTransferZoneRequest(
  URedwoodPlayerStateComponent *PlayerStateComponent,
  APlayerController *PlayerController,
  const TSharedPtr<FJsonObject> &Payload
) {
  TWeakObjectPtr<APlayerController> WeakPlayerController(PlayerController);

  // -1 and not 0: a component that never transferred still sits at
  // generation 0, and must not match an emit that ran without a component.
  int64 EmitGeneration = -1;

  if (IsValid(PlayerStateComponent)) {
    EmitGeneration = PlayerStateComponent->TransferGeneration;

    // The game instance owns this timer manager, the same one the sidecar
    // updates use; the answer must be cleared on the same one. See
    // HandleTransferAnswerTimeout for what happens when no answer comes.
    GetGameInstance()->GetTimerManager().SetTimer(
      PlayerStateComponent->TransferAnswerTimeout,
      FTimerDelegate::CreateWeakLambda(
        this,
        [this, WeakPlayerController, EmitGeneration]() {
          HandleTransferAnswerTimeout(WeakPlayerController, EmitGeneration);
        }
      ),
      TransferAnswerTimeoutSeconds,
      false // no loop
    );
  }

  Sidecar->Emit(
    TEXT("realm:servers:transfer-zone:game-server-to-sidecar"),
    Payload,
    [this, WeakPlayerController, EmitGeneration](auto Response) {
      const TSharedPtr<FJsonObject> *ResponseObject =
        URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
      HandleTransferZoneResponse(
        ResponseObject ? *ResponseObject : nullptr,
        WeakPlayerController,
        EmitGeneration
      );
    }
  );
}

// FORK(hollowed-oath): the answer callback and the timeout both start from a
// controller they captured long ago; one resolver keeps their component
// lookups identical.
static URedwoodPlayerStateComponent *ResolveRedwoodTransferComponent(
  APlayerController *PlayerController
) {
  return IsValid(PlayerController) && IsValid(PlayerController->PlayerState)
    ? PlayerController->PlayerState
        ->FindComponentByClass<URedwoodPlayerStateComponent>()
    : nullptr;
}

// FORK(hollowed-oath): shared handling of the sidecar's answer to both
// TravelPlayerToZone* emits.
//
// Upstream kicks the player on EVERY error. That is too strong: most errors
// happen before the realm takes the transfer, so the player can stay in this
// zone. The sidecar now stamps "ambiguous" on every error answer: false when
// it knows the realm never took the transfer, true when it lost contact with
// the realm during the request and the realm can have moved the character
// already.
//
//   ambiguous or ABSENT -> keep the transferring flag and kick. The kick's
//                    Logout must run the transferring teardown (no linkdead
//                    retention), because a retained body could duplicate a
//                    character the realm already re-homed. An absent field
//                    means an old sidecar that does not send it; the kick is
//                    the safe answer, so a game server can run against an
//                    older backend without a risk of duplication.
//   explicitly false -> the transfer never started. AbortTransferring rolls
//                    the flag back and tells the player, who stays in this
//                    zone.
//
// The callback is asynchronous, so the player can be gone by the time it
// runs; that is checked here, and KickPlayerAfterFailedTransfer checks the
// world and the game mode.
void URedwoodServerGameSubsystem::HandleTransferZoneResponse(
  const TSharedPtr<FJsonObject> &Response,
  TWeakObjectPtr<APlayerController> WeakPlayerController,
  int64 EmitGeneration
) {
  if (!Response.IsValid()) {
    // The sidecar answered with nothing, or with a value that is not an
    // object. Treat it as no answer at all: the wait set beside the emit
    // stays armed, and HandleTransferAnswerTimeout takes the player out.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("The sidecar gave no usable answer to a transfer request")
    );
    return;
  }

  APlayerController *PlayerController = WeakPlayerController.Get();
  URedwoodPlayerStateComponent *PlayerStateComponent =
    ResolveRedwoodTransferComponent(PlayerController);

  // FORK(hollowed-oath): true while the transfer this answer belongs to is
  // still the transfer in flight. The answer and the realm's failure report
  // travel different paths and hold no order between them, so an answer can
  // land after its own transfer is over.
  //
  // The report can really come first. The transfer route (RedwoodBackend
  // packages/realm-backend/src/routes/sidecar/transfer-zone.ts) puts the
  // join ticket in the queue BEFORE it answers, so a ticketing worker in
  // another process can fail the ticket and send the transfer-failed
  // report while the answer is still on its way here. This server always
  // asks for one character, so the route's earlier reports cannot reach
  // it: a refused single character comes back as an error answer instead.
  //
  // Two orderings need this:
  //   1. the report aborts transfer A, then A's SUCCESS answer lands on an
  //      idle component and writes A's id there;
  //   2. the report aborts A, transfer B starts, and only THEN A's answer
  //      lands — writing A's id over B's empty slot, or, if A's answer is
  //      an error, aborting B.
  // In both, B loses: its own report is dropped for a name it never had, or
  // its rollback fires for another transfer's failure, and the player stays
  // latched as transferring — the state this whole rollback exists to
  // prevent. Clearing the id in InitTransferring closes ordering 1 only,
  // because in ordering 2 A's answer arrives after that clear.
  //
  // This gates only the two branches that WRITE state: keeping the id and
  // rolling the transfer back. The kick below stays unconditional on
  // purpose — an ambiguous answer says the realm may hold the character
  // already, and that danger does not go away because the player has since
  // asked for another zone. A missed kick can duplicate a character; a
  // needless kick cannot.
  const bool bAnswerIsCurrent = IsValid(PlayerStateComponent) &&
    PlayerStateComponent->TransferGeneration == EmitGeneration;

  // FORK(hollowed-oath): the answer came, so the wait for it is over. Every
  // branch below counts as an answer, the good one too: a transfer that the
  // realm took keeps the latch on purpose until the player leaves, and the
  // timeout must not take that player out.
  if (bAnswerIsCurrent) {
    GetGameInstance()->GetTimerManager().ClearTimer(
      PlayerStateComponent->TransferAnswerTimeout
    );
  }

  // Read it quietly, like HandleTransferFailedEvent below: an answer that
  // carries no error field is a success, and GetStringField logs an engine
  // warning for a field that is not there.
  FString Error;
  Response->TryGetStringField(TEXT("error"), Error);

  if (Error.IsEmpty()) {
    // FORK(hollowed-oath): the realm took the transfer and named it. Keep
    // the name, so a later transfer-failed report can be matched to this
    // transfer. The field is absent on an older backend, and
    // TryGetStringField then leaves the id as it is (empty).
    //
    // A failure report can legally arrive BEFORE this answer: the report
    // comes on the realm socket and the answer on the request path, and
    // the two hold no order. That report is still safe, because
    // InitTransferring latched bTransferring before the emit, and an empty
    // id matches every report. The id only makes the match tighter once
    // this answer lands.
    if (bAnswerIsCurrent) {
      Response->TryGetStringField(
        TEXT("transferId"), PlayerStateComponent->ActiveTransferId
      );
    } else if (IsValid(PlayerStateComponent)) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT(
          "Not keeping the name from the answer of a transfer that is over; the player is on transfer %lld now"
        ),
        PlayerStateComponent->TransferGeneration
      );
    }

    return;
  }

  // Missing field = old sidecar; keep the safe answer.
  bool bAmbiguous = true;
  Response->TryGetBoolField(TEXT("ambiguous"), bAmbiguous);

  if (!bAmbiguous) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Failed to transfer player to new zone, keeping them here: %s"),
      *Error
    );

    // Two gates: the answer must belong to the transfer in flight (see
    // bAnswerIsCurrent above), and the player must still be transferring.
    // Without the first gate the answer of a transfer that is over rolls
    // back the transfer that runs now.
    if (bAnswerIsCurrent && PlayerStateComponent->bTransferring) {
      // AbortTransferring also clears ActiveTransferId.
      PlayerStateComponent->AbortTransferring(Error, TEXT("realm-rejected"));
    } else if (IsValid(PlayerStateComponent)) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT(
          "Not rolling back: the answer is for a transfer that is over, or the player is not transferring"
        )
      );
    }

    return;
  }

  UE_LOG(
    LogRedwood,
    Error,
    TEXT("Failed to transfer player to new zone, kicking them now: %s"),
    *Error
  );

  KickPlayerAfterFailedTransfer(PlayerController, Error);
}

// FORK(hollowed-oath): the one kick a failed transfer uses. The callback and
// the timer both run long after their transfer began, so the player, the
// world and the game mode can all be gone; every one of them is checked.
void URedwoodServerGameSubsystem::KickPlayerAfterFailedTransfer(
  APlayerController *PlayerController, const FString &Error
) {
  if (!IsValid(PlayerController)) {
    // The player left already, which is the outcome the kick wanted.
    return;
  }

  UWorld *World = GetWorld();
  AGameModeBase *GameMode = IsValid(World) ? World->GetAuthGameMode() : nullptr;

  // KickPlayer answers false and does NOTHING for a player with no net
  // connection to close (a listen host, or a connection already torn
  // down), so its answer must be read, not assumed.
  if (IsValid(GameMode) && IsValid(GameMode->GameSession) &&
      GameMode->GameSession->KickPlayer(
        PlayerController, FText::FromString(Error)
      )) {
    return;
  }

  // The player is still here, but there is nothing to kick them with. Say
  // so: the caller logged that it kicks the player, and this is the one case
  // where that does not happen. The player stays connected and stays marked
  // as transferring, and the realm can hold the character already.
  UE_LOG(
    LogRedwood,
    Error,
    TEXT(
      "Could not kick the player; this server has no game session, or the player has no net connection to close"
    )
  );
}

// FORK(hollowed-oath): the transfer got no answer. See the rationale block
// on the declaration in the header: a sidecar that dies after the connection
// test gives neither an answer nor a failure event, so without this the
// player keeps the transferring latch for the rest of the session. An answer
// that never came is ambiguous — the realm may hold the character already —
// so this ends the same way an ambiguous answer does, with a kick.
//
// Two guards keep the timer from touching the wrong transfer: the generation
// must still be the one that started this wait, and the player must still be
// transferring. HandleTransferZoneResponse clears the timer as soon as an
// answer for this transfer arrives, so a transfer that ends well never
// reaches this.
void URedwoodServerGameSubsystem::HandleTransferAnswerTimeout(
  TWeakObjectPtr<APlayerController> WeakPlayerController, int64 EmitGeneration
) {
  APlayerController *PlayerController = WeakPlayerController.Get();
  URedwoodPlayerStateComponent *PlayerStateComponent =
    ResolveRedwoodTransferComponent(PlayerController);

  if (!IsValid(PlayerStateComponent) ||
      PlayerStateComponent->TransferGeneration != EmitGeneration ||
      !PlayerStateComponent->bTransferring) {
    return;
  }

  const FString Error =
    TEXT("The zone transfer got no answer; the server cannot keep you here");

  UE_LOG(
    LogRedwood,
    Error,
    TEXT(
      "A zone transfer got no answer in %.0f seconds, kicking the player now"
    ),
    TransferAnswerTimeoutSeconds
  );

  KickPlayerAfterFailedTransfer(PlayerController, Error);
}

// FORK(hollowed-oath): body of the transfer-failed sidecar event, which
// upstream does not have. The realm sends it when a transfer it had already
// accepted then failed, so the answer to the transfer-zone route was empty
// and only this tells the server that the travel stopped. Without it the
// player keeps the transferring latch for the rest of the session.
//
// Payload: { playerId, characterId, transferId, error, reason }, written by
// RedwoodBackend packages/common/src/interfaces.ts,
// TransferFailed.RealmToSidecar.SRequest. C++ and TypeScript cannot share a
// constant, so that schema and this reader must be changed together.
// characterId is the ONLY match key: a character is on exactly one game
// server, while a player can hold several characters, so playerId is for
// the log alone — do not add a playerId match. Reason is the backend's
// kebab-case token (see AbortTransferring in RedwoodPlayerStateComponent.h);
// it goes to the game unchanged.
//
// Three gates before the abort, in order: the character must be on this
// server, the player must still be transferring (bTransferring), and the
// report must name the transfer in flight (MatchesActiveTransfer).
void URedwoodServerGameSubsystem::HandleTransferFailedEvent(
  const TSharedPtr<FJsonObject> &Payload
) {
  if (!Payload.IsValid()) {
    return;
  }

  // Read every field quietly: GetStringField logs an engine error for a
  // field that a malformed payload does not carry.
  FString CharacterId;
  FString Error;
  FString Reason;
  FString TransferId;
  Payload->TryGetStringField(TEXT("error"), Error);
  Payload->TryGetStringField(TEXT("reason"), Reason);
  Payload->TryGetStringField(TEXT("transferId"), TransferId);

  // Without a character id there is no player to tell.
  if (!Payload->TryGetStringField(TEXT("characterId"), CharacterId) ||
      CharacterId.IsEmpty()) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("The realm reported a failed transfer with no character id")
    );
    return;
  }

  UWorld *World = GetWorld();
  AGameStateBase *GameState = IsValid(World) ? World->GetGameState() : nullptr;

  if (!IsValid(GameState)) {
    return;
  }

  // Gate 1: the character must be on this server.
  URedwoodPlayerStateComponent *PlayerStateComponent = nullptr;
  for (APlayerState *PlayerState : GameState->PlayerArray) {
    URedwoodPlayerStateComponent *Candidate =
      IsValid(PlayerState)
      ? PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>()
      : nullptr;

    if (IsValid(Candidate) && Candidate->RedwoodCharacter.Id == CharacterId) {
      PlayerStateComponent = Candidate;
      break;
    }
  }

  if (!PlayerStateComponent) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "The realm reported a failed transfer for character %s, but no player on this server matches: %s"
      ),
      *CharacterId,
      *Error
    );
    return;
  }

  // Gate 2: a report for a player who is not transferring is stale or
  // wrong; an abort here would fire rollback events for a transfer that
  // does not exist.
  if (!PlayerStateComponent->bTransferring) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "The realm reported a failed transfer for character %s, but the player is not transferring: %s"
      ),
      *CharacterId,
      *Error
    );
    return;
  }

  // Gate 3: drop a report that names a different transfer; absence on
  // either side must never block the abort (see MatchesActiveTransfer).
  if (!PlayerStateComponent->MatchesActiveTransfer(TransferId)) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "Ignoring a failed transfer report for character %s: it names transfer %s, but the player is on transfer %s"
      ),
      *CharacterId,
      *TransferId,
      *PlayerStateComponent->ActiveTransferId
    );
    return;
  }

  UE_LOG(
    LogRedwood,
    Warning,
    TEXT("The realm could not transfer character %s (%s): %s"),
    *CharacterId,
    *Reason,
    *Error
  );

  PlayerStateComponent->AbortTransferring(Error, Reason);
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
    // FORK(hollowed-oath): InitTransferring ran above, so this failed path
    // must roll the flag back or the player stays latched as transferring
    // for the rest of the session. See AbortTransferring's rationale block.
    if (PlayerStateComponent) {
      PlayerStateComponent->AbortTransferring(
        SidecarDownError, SidecarDownReason
      );
    }
    return;
  }

  UE_LOG(
    LogRedwood,
    Log,
    TEXT("Traveling player %s to zone %s"),
    *PlayerId,
    *InZoneName
  );

  // FORK(hollowed-oath): the same wait and emit as
  // TravelPlayerToZoneTransform above; see EmitTransferZoneRequest.
  EmitTransferZoneRequest(PlayerStateComponent, PlayerController, Payload);
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
  // The delivery guarantee that note asked for now exists: a parked flush re-runs on the in-flight
  // batch's ack and carries the barrier's settle with it (#1534).
  if (!bUseBackend) {
    // Offline: item rows go inline into the on-disk character JSON, mirroring
    // CreatePlayerCharacterDataObject's offline leg (a bare FlushItemsForCharacterComponent would
    // no-op with no sidecar and drop the rows). No barrier needed -- the disk write is synchronous
    // and the binding release is a no-op offline (EmitPlayerLeft short-circuits without a backend).
    TArray<TSharedPtr<FJsonValue>> DetachedOfflineItemRows;
    if (AppendOfflineItemRows(DetachedOfflineItemRows, CharacterComponent)) {
      CharacterObject->SetArrayField(TEXT("items"), DetachedOfflineItemRows);
    }
    // FORK(hollowed-oath) BEGIN: carry the InventorySeq fence across the detached offline write too,
    // for the same reason CreatePlayerCharacterDataObject does -- this is a whole-file rewrite, so the
    // seq must ride through as whatever the struct holds. The fence lives on the PlayerState's
    // RedwoodCharacter, not the CharacterComponent; a detached (linkdead) pawn may have no live
    // PlayerState, so resolve it best-effort through the pawn's controller. NOTE: offline linkdead
    // retention is dormant today (EmitPlayerLeft short-circuits without a backend and the retention
    // manager stays dormant until the zone game mode is reparented), so this path is not exercised in
    // practice; the seq fence is meaningless offline (0) anyway. Row-per-item is the native model now,
    // so there is no migration marker to carry -- deleted with the migrate route.
    APlayerState *DetachedPlayerState =
      IsValid(Pawn) && IsValid(Pawn->GetController())
      ? Pawn->GetController()->PlayerState
      : nullptr;
    URedwoodPlayerStateComponent *DetachedPSC =
      IsValid(DetachedPlayerState)
      ? DetachedPlayerState->FindComponentByClass<URedwoodPlayerStateComponent>()
      : nullptr;
    if (DetachedPSC) {
      CharacterObject->SetNumberField(
        TEXT("inventorySeq"), DetachedPSC->RedwoodCharacter.InventorySeq
      );
      // FORK(hollowed-oath): the staged currency goes into the same write, as on the attached
      // path.
      ApplyOfflineCurrency(DetachedPSC->RedwoodCharacter, CharacterComponent);
      URedwoodCommonGameSubsystem::WriteOfflineCurrency(
        DetachedPSC->RedwoodCharacter, CharacterObject
      );
    } else if (CharacterComponent->GetPendingCurrencyEntries().Num() > 0 ||
               CharacterComponent->GetPendingGrantKeys().Num() > 0) {
      // FORK(hollowed-oath): with no PlayerState there is no balance to apply the entries to, so
      // they go with the pawn, loudly. SaveCharacterJsonToDisk keeps the file's balance.
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Offline detached save: character %s has no PlayerState; %d currency entries and %d grant keys are NOT saved"),
        *CharacterId,
        CharacterComponent->GetPendingCurrencyEntries().Num(),
        CharacterComponent->GetPendingGrantKeys().Num()
      );
    }
    // FORK(hollowed-oath) END
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

  // FORK(hollowed-oath): the settled-latch now orders the binding release behind a real DELIVERY.
  // A flush attempt that finds a batch already in flight no longer settles on the spot: it parks
  // itself on the component and re-runs when that batch's ack frees the slot (see the single-flight
  // branch in FlushItemsForCharacterComponent, and CompleteItemFlush which runs the parked attempt),
  // carrying THIS latch's settle with it. So EmitPlayerLeft can no longer release the write binding
  // while a detached pawn's dirty rows are still sitting in the process (#1534).
  //
  // What remains outside this guarantee, deliberately: a backend REJECTION. The ack settles the
  // latch whatever the outcome, because the alternative is a detached pawn whose binding never
  // releases at all. A rejected batch leaves its dirt marked and is reconciled from the backend rows
  // on the next login -- the same recovery every error path here relies on.
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

    // FORK(hollowed-oath): at most ONE item flush per CHARACTER per save pass. The single-flight
    // slot and the batchSeq counter live on the COMPONENT, but the sequence they fence is
    // per-CHARACTER on the backend. With both a PlayerState-owned and a pawn-owned component
    // carrying items for the same character, each would claim the same seq: the backend commits one
    // and no-ops the other as a replay, and that second component then clears its rows on the
    // success-shaped ack -- losing them. Rows are re-derived from live storage on the next flush, so
    // deferring the second component costs nothing.
    TSet<FString> ItemFlushedCharacterIds;

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

      // FORK(hollowed-oath): the item channel must run on THIS branch too. Items never travel in the
      // character payload -- they go out on their own sidecar route -- so taking the snapshot branch
      // skipped them entirely. On a logout save (bForce) with a PlayerState override pending, the
      // component was then destroyed with its dirty rows never sent. The per-component loop in the
      // else-branch is the only other place the item flush happens, and it is not reached from here.
      if (bUseBackend) {
        for (URedwoodCharacterComponent *ItemComponent : CharacterComponents) {
          if (IsValid(ItemComponent) && ItemComponent->IsItemsDirty()
              && !ItemFlushedCharacterIds.Contains(ItemComponent->RedwoodCharacterId)) {
            ItemFlushedCharacterIds.Add(ItemComponent->RedwoodCharacterId);
            FlushItemsForCharacterComponent(PlayerStateComponent, ItemComponent);
          }
        }
      }

      // FORK(hollowed-oath) BEGIN: hand the override off to the character components, then consume it.
      // What: clear bCharacterDataDirty here (ClearDirtyFlags existed but had NO caller anywhere).
      // Why: the flag is LATCHING. Anything that sets it -- the game's linkdead-reclaim path in
      //   AHollowedOathGameMode calls MarkCharacterDataDirty() when it folds a retained body's
      //   metadata back in -- made EVERY subsequent save for that player take this branch forever.
      //   The per-component else-branch below is where the fork's item flush lives, so the character
      //   would never persist another item mutation: loot taken after a reclaim vanished on relog,
      //   and consumed items came back. The broadcast immediately above is what makes clearing safe
      //   -- it exists precisely so the character components sync to this data, after which they are
      //   the correct source for the next save.
      // Preserve on upstream merge: upstream has no item flush and so never noticed the missing
      //   clear; keep this call (or upstream's equivalent) alive.
      //
      // Clearing here is only safe because the data does NOT rest on this save succeeding. The
      // backend write below is a fire-and-forget Emit with no ack, so if the batch were lost and the
      // latch were simply dropped, the overridden groups (a retained body's metadata — health taken
      // before the first save after a reclaim) would have no retry path: same-schema deserialization
      // does not re-dirty them. So TRANSFER the override into the components' own dirty flags first.
      // They have just synced to this data via the broadcast above, so they now carry it, and the
      // next save re-sends it through the ordinary per-component path until it lands.
      for (URedwoodCharacterComponent *CharacterComponent : CharacterComponents) {
        if (!IsValid(CharacterComponent)) {
          continue;
        }
        // EVERY component-backed group the snapshot can override, not just the four obvious ones:
        // a lost emit must be retriable for all of them, and same-schema deserialization during the
        // broadcast above does not mark the omitted ones dirty by itself.
        CharacterComponent->MarkCharacterCreatorDataDirty();
        CharacterComponent->MarkMetadataDirty();
        CharacterComponent->MarkEquippedInventoryDirty();
        CharacterComponent->MarkNonequippedInventoryDirty();
        CharacterComponent->MarkProgressDirty();
        CharacterComponent->MarkDataDirty();
        CharacterComponent->MarkAbilitySystemDirty();
      }
      PlayerStateComponent->ClearDirtyFlags();
      // FORK(hollowed-oath) END

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
          // One flush per character per pass -- see ItemFlushedCharacterIds above.
          if (CharacterComponent->IsItemsDirty()
              && !ItemFlushedCharacterIds.Contains(CharacterComponent->RedwoodCharacterId)) {
            ItemFlushedCharacterIds.Add(CharacterComponent->RedwoodCharacterId);
            FlushItemsForCharacterComponent(
              PlayerStateComponent, CharacterComponent
            );
          }
        } else {
          if (AppendOfflineItemRows(OfflineItemRows, CharacterComponent)) {
            bHasOfflineItemRows = true;
          }
          // The staged currency moves onto the snapshot, which the offline block below writes.
          ApplyOfflineCurrency(PlayerStateComponent->RedwoodCharacter, CharacterComponent);
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

    // FORK(hollowed-oath) BEGIN: persist the InventorySeq fence into the offline character JSON
    // (fork-added; upstream has no item-persistence concept). This is the runtime offline write path
    // (FlushPlayerCharacterData -> SaveCharacterJsonToDisk on the periodic timer and the forced logout
    // flush), mirroring SaveCharacterToDisk (which handles character creation). Gated on !bUseBackend
    // for the same reason the item rows are: the backend leg keeps the seq on the character row and
    // must not fold it into realm:characters:set:server. This does NOT set bIsDirty -- it is metadata
    // riding an already-dirty save. inventorySeq echoes the DB column for wire parity; the fence is
    // meaningless offline (0), see the SaveCharacterToDisk FORK note. Row-per-item is the native model
    // now, so there is no migration marker to write here -- deleted with the migrate route.
    if (!bUseBackend) {
      CharacterObject->SetNumberField(
        TEXT("inventorySeq"), PlayerStateComponent->RedwoodCharacter.InventorySeq
      );
      // The balance and grant keys ride the same whole-file write, from the snapshot the
      // component loop above applied the staged currency to.
      URedwoodCommonGameSubsystem::WriteOfflineCurrency(
        PlayerStateComponent->RedwoodCharacter, CharacterObject
      );
    }
    // FORK(hollowed-oath) END

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

// FORK(hollowed-oath) BEGIN: item flush + trade + offline-append helpers, entirely
// fork-added (no upstream counterpart). Replaces the earlier per-container flush helpers
// (FlushContainersForCharacterComponent / AppendOfflineContainerRows, RedwoodPlugins#17) now that
// persistence is per-item flat rows instead of opaque per-container blobs. Backend counterpart:
// RedwoodBackend feat/item-persistence (realm:characters:items:{flush,trade} routes). Row-per-item
// is the native model now, so there is no blob->row migration route/helper -- the earlier
// EmitItemsMigrate / BuildItemsMigratePayload / realm:characters:items:migrate surface is deleted.
//
// FlushItemsForCharacterComponent is the per-item persistence channel, protocol v2: single-flight
// + seq-fenced. It sends ONLY currently-dirty item records (plus pending deletions) to
// realm:characters:items:flush under a batchSeq claimed by TryBeginItemFlush, and clears dirty
// state ONLY on a generation-matched ack inside CompleteItemFlush, so a re-dirty mid-flight is
// never stranded and a failed/lost/component-destroyed send is always retried. At most one batch
// is outstanding: while one is in flight TryBeginItemFlush returns false and this tick's flush is
// parked (dirt keeps coalescing). AppendOfflineItemRows is its offline/PIE twin (whole-array write
// into the on-disk character JSON). EmitItemsTrade is a one-shot backend op that holds both
// characters' flushes until it settles, then hands each side its commit. Called from CreatePlayerCharacterDataObject /
// FlushDetachedCharacterData above; the game module (AClient / per-bag inventory) drives the dirty
// state via URedwoodCharacterComponent::MarkItemsDirty/MarkItemsDeleted and calls the trade
// emit. Declarations sit under a matching FORK marker in RedwoodServerGameSubsystem.h.
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
// The shared envelope contract for the two item emits below, flush and trade (see the header FORK
// block): every
// payload carries the "game-server" placeholder id because the sidecar validates the FULL request
// schema (id: string().required()) BEFORE stamping its own instanceId over the field -- identical
// to the realm:characters:set:server payload above. A null or absent id fails that pre-stamp
// validation ("id is a required field") and the message never reaches the realm; ItemPersistenceTest
// pins these builders so the envelope layer can no longer regress untested.
// file-local (static, not an anonymous namespace -- those merge across unity-build blobs)
static const TCHAR *ItemsEnvelopePlaceholderId = TEXT("game-server");

// FORK(hollowed-oath): one currency entry on the wire, for the flush and the trade. Amounts go as
// decimal strings: a JSON number is a double, which cannot hold every int64, so a large amount
// would arrive rounded. Optional fields are left out when empty: the backend schema accepts a
// missing field, but not null.
static TSharedPtr<FJsonObject> MakeCurrencyEntryJson(const FRedwoodCurrencyEntry &Entry) {
  TSharedPtr<FJsonObject> EntryObject = MakeShareable(new FJsonObject);
  EntryObject->SetStringField(TEXT("id"), Entry.Id);
  EntryObject->SetStringField(TEXT("delta"), LexToString(Entry.Delta));
  EntryObject->SetStringField(TEXT("balanceAfter"), LexToString(Entry.BalanceAfter));
  EntryObject->SetStringField(TEXT("reason"), Entry.Reason);
  if (!Entry.SourceRef.IsEmpty()) {
    EntryObject->SetStringField(TEXT("sourceRef"), Entry.SourceRef);
  }
  if (!Entry.ActorAccountId.IsEmpty()) {
    EntryObject->SetStringField(TEXT("actorAccountId"), Entry.ActorAccountId);
  }
  return EntryObject;
}

// FORK(hollowed-oath): the `alerts` of a flush or trade reply. An entry that is not an object is
// skipped.
static void ParseCurrencyAlerts(
  const TSharedPtr<FJsonObject> &Reply, TArray<TSharedPtr<FJsonObject>> &OutAlerts
) {
  const TArray<TSharedPtr<FJsonValue>> *AlertValues = nullptr;
  if (!Reply->TryGetArrayField(TEXT("alerts"), AlertValues)) {
    return;
  }
  for (const TSharedPtr<FJsonValue> &AlertValue : *AlertValues) {
    const TSharedPtr<FJsonObject> *AlertObject = nullptr;
    if (AlertValue.IsValid() && AlertValue->TryGetObject(AlertObject) && AlertObject) {
      OutAlerts.Add(*AlertObject);
    }
  }
}

TSharedPtr<FJsonObject> URedwoodServerGameSubsystem::BuildItemsFlushPayload(
  const FString &CharacterId,
  int64 BatchSeq,
  const TArray<TSharedPtr<FJsonValue>> &Upserts,
  const TArray<TSharedPtr<FJsonValue>> &Deletes,
  const TArray<FRedwoodCurrencyEntry> &CurrencyEntries,
  const TArray<FString> &GrantKeys,
  const FString &WriterToken,
  bool bProbe
) {
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), ItemsEnvelopePlaceholderId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);
  Payload->SetNumberField(TEXT("batchSeq"), static_cast<double>(BatchSeq));
  Payload->SetArrayField(TEXT("upserts"), Upserts);
  Payload->SetArrayField(TEXT("deletes"), Deletes);

  TArray<TSharedPtr<FJsonValue>> CurrencyJsonArray;
  CurrencyJsonArray.Reserve(CurrencyEntries.Num());
  for (const FRedwoodCurrencyEntry &Entry : CurrencyEntries) {
    CurrencyJsonArray.Add(MakeShareable(new FJsonValueObject(MakeCurrencyEntryJson(Entry))));
  }
  Payload->SetArrayField(TEXT("currency"), CurrencyJsonArray);

  TArray<TSharedPtr<FJsonValue>> GrantsJsonArray;
  GrantsJsonArray.Reserve(GrantKeys.Num());
  for (const FString &GrantKey : GrantKeys) {
    GrantsJsonArray.Add(MakeShareable(new FJsonValueString(GrantKey)));
  }
  Payload->SetArrayField(TEXT("grants"), GrantsJsonArray);

  Payload->SetStringField(TEXT("writerToken"), WriterToken);
  // FORK(hollowed-oath): a re-send of an unconfirmed batch says so. The backend never applies a
  // probe; it only answers whether the first attempt committed. Left out otherwise (never false or
  // null), the way older senders send it.
  if (bProbe) {
    Payload->SetBoolField(TEXT("probe"), true);
  }
  return Payload;
}

TSharedPtr<FJsonObject> URedwoodServerGameSubsystem::BuildItemsTradePayload(
  const FString &FromCharacterId,
  const FString &ToCharacterId,
  const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
  const FString &FromWriterToken,
  const FString &ToWriterToken,
  const FString &TradeId
) {
  TArray<TSharedPtr<FJsonValue>> RootPlacementsJsonArray;
  RootPlacementsJsonArray.Reserve(RootPlacements.Num());
  for (const FRedwoodTradeRootPlacement &Placement : RootPlacements) {
    TSharedPtr<FJsonObject> PlacementObject = MakeShareable(new FJsonObject);
    PlacementObject->SetStringField(TEXT("id"), Placement.Id);
    PlacementObject->SetStringField(TEXT("domain"), Placement.Domain);
    PlacementObject->SetNumberField(TEXT("slot"), Placement.Slot);
    // FORK(hollowed-oath): mirror SerializeItemRecord's null convention for ParentId (empty =
    // JSON null, not ""), so the backend's trade route can treat a content-child destination
    // (nested inside a receiving-side bag item) the same way it already treats item rows --
    // production trades move items between bags, not just top-level slots.
    if (Placement.ParentId.IsEmpty()) {
      PlacementObject->SetField(TEXT("parentId"), MakeShareable(new FJsonValueNull));
    } else {
      PlacementObject->SetStringField(TEXT("parentId"), Placement.ParentId);
    }
    // FORK(hollowed-oath): always named, so the backend never has to guess the direction.
    PlacementObject->SetStringField(
      TEXT("fromCharacterId"),
      Placement.FromCharacterId.IsEmpty() ? FromCharacterId : Placement.FromCharacterId
    );
    RootPlacementsJsonArray.Add(
      MakeShareable(new FJsonValueObject(PlacementObject))
    );
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), ItemsEnvelopePlaceholderId);
  Payload->SetStringField(TEXT("fromCharacterId"), FromCharacterId);
  Payload->SetStringField(TEXT("toCharacterId"), ToCharacterId);
  Payload->SetArrayField(TEXT("rootPlacements"), RootPlacementsJsonArray);

  // FORK(hollowed-oath): each entry names its character, as the backend's trade schema reads it.
  TArray<TSharedPtr<FJsonValue>> CurrencyJsonArray;
  CurrencyJsonArray.Reserve(CurrencyByCharacter.Num());
  for (const TPair<FString, FRedwoodCurrencyEntry> &Pair : CurrencyByCharacter) {
    TSharedPtr<FJsonObject> SideObject = MakeShareable(new FJsonObject);
    SideObject->SetStringField(TEXT("characterId"), Pair.Key);
    SideObject->SetObjectField(TEXT("entry"), MakeCurrencyEntryJson(Pair.Value));
    CurrencyJsonArray.Add(MakeShareable(new FJsonValueObject(SideObject)));
  }
  Payload->SetArrayField(TEXT("currency"), CurrencyJsonArray);
  // FORK(hollowed-oath): without both tokens the backend refuses every trade; without the trade id,
  // every trade that carries currency.
  Payload->SetStringField(TEXT("fromWriterToken"), FromWriterToken);
  Payload->SetStringField(TEXT("toWriterToken"), ToWriterToken);
  Payload->SetStringField(TEXT("tradeId"), TradeId);
  return Payload;
}

// FORK(hollowed-oath): catch this component up to the character's committed fence before it
// claims a seq. The fence is per-CHARACTER but NextBatchSeq is per-COMPONENT, so a component that
// did not send the last batch -- the one deferred by the one-flush-per-character rule, or one
// created after another had already flushed -- still holds its login-time value. Sending that
// stale seq makes the backend answer "replay", and the success-shaped ack then clears rows that
// were never persisted. The PlayerState snapshot carries the latest committed fence (written back
// on every successful ack), so trust it when it is ahead. Only ever forward: seeding backwards
// would re-wedge the lane behind a seq the backend has already passed.
static void CatchUpItemSeqFromSnapshot(
  const URedwoodPlayerStateComponent *PlayerStateComponent,
  URedwoodCharacterComponent *CharacterComponent
) {
  if (!PlayerStateComponent) {
    return;
  }
  const FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  if (Snapshot.InventorySeq > CharacterComponent->GetLastCommittedItemSeq()) {
    CharacterComponent->SeedItemSeqFromCharacter(Snapshot.InventorySeq);
    // FORK(hollowed-oath): a component that catches up from a snapshot whose balance is stale
    // must not send currency chained from it.
    if (Snapshot.bCurrencyAmountUnknown) {
      CharacterComponent->RequireCurrencyReload();
    }
  }
}

// FORK(hollowed-oath): keeps a flush attempt for later. Every attempt with a settle is kept, so
// each one settles exactly once. A live tick flush has none; one entry per component is enough to
// flush it again later.
static void AddHeldItemFlush(
  TArray<FRedwoodHeldItemFlush> &HeldFlushes,
  URedwoodPlayerStateComponent *PlayerStateComponent,
  URedwoodCharacterComponent *CharacterComponent,
  TFunction<void()> OnAckSettled
) {
  const bool bAlreadyHeldLive = !OnAckSettled &&
    HeldFlushes.ContainsByPredicate([CharacterComponent](const FRedwoodHeldItemFlush &Held) {
      return !Held.OnAckSettled && Held.CharacterComponent.Get() == CharacterComponent;
    });
  if (bAlreadyHeldLive) {
    return;
  }
  FRedwoodHeldItemFlush &Held = HeldFlushes.AddDefaulted_GetRef();
  Held.CharacterComponent = CharacterComponent;
  Held.PlayerStateComponent = PlayerStateComponent;
  Held.OnAckSettled = MoveTemp(OnAckSettled);
}

void URedwoodServerGameSubsystem::ResumeHeldItemFlushes(TArray<FRedwoodHeldItemFlush> &HeldFlushes) {
  // Single-flight still holds: the first resumed attempt of a component claims its slot, a second
  // parks behind it, and any more settle at once, exactly as un-held attempts would. A live
  // component's entry flushes it now, so what waited goes out without waiting for the next save
  // pass.
  for (FRedwoodHeldItemFlush &Held : HeldFlushes) {
    URedwoodCharacterComponent *Component = Held.CharacterComponent.Get();
    if (IsValid(Component)) {
      FlushItemsForCharacterComponent(
        Held.PlayerStateComponent.Get(), Component, MoveTemp(Held.OnAckSettled)
      );
    } else if (Held.OnAckSettled) {
      Held.OnAckSettled();
    }
  }
}

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

  // FORK(hollowed-oath): a component that was never hydrated has no character to write for, and
  // must never join a character's hold or reload (an empty id would join every other such one).
  const FString CharacterId = PlayerStateComponent ? PlayerStateComponent->RedwoodCharacter.Id
                                                   : CharacterComponent->RedwoodCharacterId;
  if (CharacterId.IsEmpty()) {
    if (CharacterComponent->IsItemsDirty()) {
      UE_LOG(
        LogRedwood, Error, TEXT("FlushItemsForCharacterComponent: the component has no character id; not flushing")
      );
    }
    Settle();
    return;
  }

  // FORK(hollowed-oath): another session owns this character (see IsWriterFenceLost). Never send
  // again, not even a probe; settle so a detached pawn's barrier releases.
  if (CharacterComponent->IsWriterFenceLost()) {
    Settle();
    return;
  }

  // FORK(hollowed-oath): a final refusal froze this character. Nothing staged can be trusted, so
  // whatever this component staged since is dropped (and logged), and the attempt settles with
  // nothing left to save: a detached pawn's write binding releases as usual. With a batch of this
  // component in flight, its reply drops instead, AFTER it has removed what committed: dropping
  // now would log saved entries as lost, and staff would pay them twice.
  if (CharacterComponent->IsFrozenAfterFinalRefusal() || FrozenCharacterIds.Contains(CharacterId)) {
    if (!CharacterComponent->IsItemFlushInFlight()) {
      CharacterComponent->FreezeAfterFinalRefusal(TEXT("the character is frozen"));
    }
    Settle();
    return;
  }

  // FORK(hollowed-oath): an unsettled trade owns this character's lane. Its commit takes the next
  // seq and moves the balance: a batch sent meanwhile could take that seq first, or carry entries
  // chained from the old balance, and a batch of item rows sent at a seq the trade used is acked as
  // a replay and never saved. Held even with nothing dirty, so a detached pawn's write binding is
  // not released under the trade. The trade resumes this attempt, settle and all, when it settles.
  if (FRedwoodPendingItemTrade *Trade = FindItemTradeOfCharacter(CharacterId)) {
    AddHeldItemFlush(
      Trade->HeldFlushes, PlayerStateComponent, CharacterComponent, MoveTemp(OnAckSettled)
    );
    return;
  }

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

  CatchUpItemSeqFromSnapshot(PlayerStateComponent, CharacterComponent);

  // FORK(hollowed-oath): while a reload is needed for this character, hold EVERY flush of it, from
  // every component, item rows included. A purchase is its item row plus its debit; with the debit
  // held for the reload, the row alone could commit, and if the debit is then refused for good the
  // item is free. The flag is never set while this component has a batch in flight, so no reply is
  // coming that would start the reload: start or join it now. Its reply resumes this attempt with
  // its settle, so a detached pawn's barrier waits for the delivery rather than for the hold.
  if (CharacterComponent->IsCurrencyRebaseRequired() || CurrencyReloads.Contains(CharacterId)) {
    HoldItemFlushForCurrencyReload(
      CharacterId, PlayerStateComponent, CharacterComponent, OnAckSettled
    );
    return;
  }

  // Claim the single-flight slot + the batchSeq this batch must stamp. If a batch is already
  // outstanding, park this tick's flush -- the dirt stays marked and coalesces until the in-flight
  // batch's ack releases the slot. Settle so the detached barrier doesn't wait forever on a parked
  // tick. For a LIVE component the still-dirty rows retry on that component's next tick-flush; a
  // DETACHED pawn has no next tick, so a parked attempt is DEFERRED rather than settled: it re-runs
  // from CompleteItemFlush once this batch's ack frees the slot, carrying the detached barrier's
  // settle with it, so the write binding cannot release before the rows land (#1534).
  int64 BatchSeq = 0;
  if (!CharacterComponent->TryBeginItemFlush(BatchSeq)) {
    // PARK, don't settle. A live component would retry on its next tick, but a DETACHED pawn has no
    // next tick: settling here let the release barrier reach zero and drop the write binding while
    // these rows had never left the process (#1534). Defer the whole attempt — including its settle
    // — until the in-flight batch's ack frees the slot; CompleteItemFlush runs it. The barrier then
    // waits on a real DELIVERY rather than on an attempt.
    //
    // Only one attempt is parked. A second one settles immediately as before: that is the old
    // interim behaviour, and it is strictly better than growing an unbounded retry chain on a
    // component that may be about to be destroyed.
    if (!CharacterComponent->HasPendingItemFlushRetry()) {
      TWeakObjectPtr<URedwoodServerGameSubsystem> WeakSubsystem(this);
      TWeakObjectPtr<URedwoodPlayerStateComponent> WeakPlayerState(PlayerStateComponent);
      TWeakObjectPtr<URedwoodCharacterComponent> WeakCharacter(CharacterComponent);
      TFunction<void()> DeferredSettle = OnAckSettled;
      CharacterComponent->SetPendingItemFlushRetry(
        [WeakSubsystem, WeakPlayerState, WeakCharacter, DeferredSettle]() {
          if (!WeakSubsystem.IsValid() || !WeakCharacter.IsValid()) {
            // The component or subsystem went away before the slot freed; settle so a barrier
            // waiting on this attempt can never wedge.
            if (DeferredSettle) {
              DeferredSettle();
            }
            return;
          }
          WeakSubsystem->FlushItemsForCharacterComponent(
            WeakPlayerState.Get(), WeakCharacter.Get(), DeferredSettle
          );
        }
      );
      return;
    }
    Settle();
    return;
  }

  // FORK(hollowed-oath): everything this batch sends and its reply needs, copied now. The queues
  // and the dirty maps can change before the reply arrives.
  FRedwoodItemFlushSent Sent;
  Sent.BatchSeq = BatchSeq;

  // FORK(hollowed-oath): the currency this batch carries, taken right after the claim because the
  // claim decides it (an unconfirmed batch goes out again exactly as it was sent). The reply must
  // name the same counts, so CompleteItemFlush drops exactly what went out.
  TArray<FRedwoodCurrencyEntry> SentCurrencyEntries;
  TArray<FString> SentGrantKeys;
  ClaimCurrencyToSend(CharacterComponent, SentCurrencyEntries, SentGrantKeys);
  Sent.CurrencyCount = SentCurrencyEntries.Num();
  Sent.GrantKeys = SentGrantKeys;
  if (SentCurrencyEntries.Num() > 0) {
    Sent.bHasBalanceAfter = true;
    Sent.BalanceAfter = SentCurrencyEntries.Last().BalanceAfter;
  }

  // FORK(hollowed-oath): a re-send of an unconfirmed currency batch carries no item rows. If the
  // first attempt committed, the backend answers this one as a replay and applies none of its rows,
  // yet the success reply would clear them.
  const bool bReplayProbe = CharacterComponent->IsItemFlushReplayProbe();

  // Generation counters, not plain ids -- lets CompleteItemFlush tell "the record I sent" apart
  // from "the record as it stands after a re-dirty that arrived while this send was in flight".
  const TMap<FString, uint64> &DirtyGenerations =
    CharacterComponent->GetDirtyItemGenerations();
  const TMap<FString, uint64> &DeletedGenerations =
    CharacterComponent->GetPendingDeletedItemGenerations();

  TArray<TSharedPtr<FJsonValue>> UpsertsJsonArray;
  TArray<TSharedPtr<FJsonValue>> DeletesJsonArray;
  if (!bReplayProbe) {
    // Sent.Upserts holds the exact (id, generation) pairs actually included in UpsertsJsonArray --
    // NOT simply DirtyGenerations, since a dirty id with no matching record (logged below) is never
    // sent and must not be acked.
    for (const FRedwoodItemRecord &Record : *RecordsArray) {
      const uint64 *Generation = DirtyGenerations.Find(Record.Id);
      if (!Generation) {
        continue;
      }

      UpsertsJsonArray.Add(MakeShareable(new FJsonValueObject(
        URedwoodCommonGameSubsystem::SerializeItemRecord(Record)
      )));
      Sent.Upserts.Add(TPair<FString, uint64>(Record.Id, *Generation));
    }

    if (UpsertsJsonArray.Num() != DirtyGenerations.Num()) {
      // A dirty item id that isn't in RecordsArray at all -- the game marked it dirty but never
      // wrote a matching record into the items array before the flush ran. Not fatal (the
      // deletion, if any, still goes out), but worth surfacing since that record is silently
      // skipped this flush. It stays dirty (not in Sent.Upserts) so a later flush, once a matching
      // record exists, retries it.
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

    for (const TPair<FString, uint64> &Deleted : DeletedGenerations) {
      DeletesJsonArray.Add(MakeShareable(new FJsonValueString(Deleted.Key)));
      Sent.Deletes.Add(Deleted);
    }
  }

  // FORK(hollowed-oath): pending currency counts as sendable. Without it a batch of entries alone
  // would take this path, and the entries would never leave.
  if (UpsertsJsonArray.Num() == 0 && DeletesJsonArray.Num() == 0 &&
      SentCurrencyEntries.Num() == 0 && SentGrantKeys.Num() == 0) {
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

  TSharedPtr<FJsonObject> Payload = BuildItemsFlushPayload(
    CharacterId,
    BatchSeq,
    UpsertsJsonArray,
    DeletesJsonArray,
    SentCurrencyEntries,
    SentGrantKeys,
    CharacterComponent->GetWriterToken(),
    bReplayProbe
  );

  TWeakObjectPtr<URedwoodCharacterComponent> WeakCharacterComponent = CharacterComponent;
  // FORK(hollowed-oath): carried into the ack so the committed seq and balance can be written back
  // onto the PlayerState snapshot -- see ApplyItemsFlushReply for why a stale snapshot loses items.
  TWeakObjectPtr<URedwoodPlayerStateComponent> WeakPlayerStateComponent = PlayerStateComponent;
  TWeakObjectPtr<URedwoodServerGameSubsystem> WeakSubsystem(this);

  // FORK(hollowed-oath): counted per character, not per component, so a trade sees a batch that
  // another component of the character has open (see PrepareItemTradeSide).
  ++InFlightItemFlushesByCharacter.FindOrAdd(CharacterId);
  Sidecar->Emit(
    TEXT("realm:characters:items:flush"),
    Payload,
    [WeakSubsystem,
     WeakCharacterComponent,
     WeakPlayerStateComponent,
     CharacterId,
     Sent,
     OnAckSettled](auto Response) {
      // FORK(hollowed-oath): an unreadable reply parses as an error with no fence: the outcome is
      // unknown, so the batch is kept and re-sent.
      const TSharedPtr<FJsonObject> *ReplyObject =
        URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
      const FRedwoodItemFlushReply Reply =
        ParseItemsFlushReply(ReplyObject ? *ReplyObject : TSharedPtr<FJsonObject>());
      // FORK(hollowed-oath): first, so a parked attempt or a trade started from this reply counts
      // the batch as answered.
      if (URedwoodServerGameSubsystem *AnsweredSubsystem = WeakSubsystem.Get()) {
        AnsweredSubsystem->EndItemFlushInFlight(CharacterId);
      }

      URedwoodCharacterComponent *CharacterComponent = WeakCharacterComponent.Get();
      if (!IsValid(CharacterComponent)) {
        // The player disconnected (or the pawn/PlayerState was destroyed) while this request was
        // in flight -- the single-flight slot lives on that now-dead component, so there is
        // nothing to release or clear. (Inherited exposure: the flush barrier that bounds a
        // detached pawn's lifetime keeps this from leaking a live component's slot.)
        if (!Reply.Error.IsEmpty()) {
          UE_LOG(
            LogRedwood, Error, TEXT("Failed to flush items (component gone): %s"), *Reply.Error
          );
        }
        // FORK(hollowed-oath): a committed batch is still committed. The component that sent it
        // is gone, but the snapshot outlives it, and the next component hydrates from it: without
        // the commit's seq, balance and grant keys there, it would reuse the seq, chain from a
        // stale balance, and stage a reward whose key is already used, which the backend refuses
        // for good. An error reply writes nothing here: what it would change lives on the
        // component that is gone.
        if (URedwoodPlayerStateComponent *LivePlayerStateComponent = WeakPlayerStateComponent.Get()) {
          WriteCommittedFlushToSnapshot(LivePlayerStateComponent->RedwoodCharacter, Reply, Sent);
        }
        // Still settle: the detached release barrier must not stall forever just because the
        // component died mid-flight -- the attempt HAS resolved.
        if (OnAckSettled) {
          OnAckSettled();
        }
        return;
      }

      if (!Reply.Error.IsEmpty()) {
        UE_LOG(LogRedwood, Error, TEXT("Failed to flush items: %s"), *Reply.Error);
      }

      // FORK(hollowed-oath): before the component takes the reply, so the parked attempt it runs
      // already sees that this session no longer owns the character.
      if (Reply.bWriterFenceLost) {
        CharacterComponent->MarkWriterFenceLost();
      }

      // FORK(hollowed-oath): a final refusal freezes the whole character, BEFORE the component
      // takes the reply. CompleteItemFlush then freezes the component (dropping and logging all it
      // staged) and tells the game; by then every other component of the character, and a parked
      // attempt it runs, already sees the freeze. This attempt then settles with nothing to save.
      if (!Reply.Error.IsEmpty() && Reply.bFinal) {
        if (URedwoodServerGameSubsystem *Subsystem = WeakSubsystem.Get()) {
          Subsystem->FreezeCharacter(CharacterId, WeakPlayerStateComponent.Get());
        }
      }

      // ALWAYS complete (success or error): CompleteItemFlush (inside) releases the single-flight
      // slot and, on success, clears only the generation-matched sent ids and advances the
      // committed seq; on error it leaves the dirt for the next flush.
      ApplyItemsFlushReply(
        CharacterComponent, WeakPlayerStateComponent.Get(), Reply, Sent
      );

      URedwoodServerGameSubsystem *Subsystem = WeakSubsystem.Get();
      // FORK(hollowed-oath): per character, so a trade sees an unknown outcome on any component.
      if (Subsystem && CharacterComponent->IsCurrencyBatchUnconfirmed()) {
        Subsystem->UnconfirmedCurrencyComponentsByCharacter.FindOrAdd(CharacterId).AddUnique(
          CharacterComponent
        );
      }
      if (CharacterComponent->IsWriterFenceLost()) {
        // Nothing more to do: no reload or freeze for a session that no longer owns the character.
      } else if (Subsystem && Subsystem->IsCharacterFrozen(CharacterId)) {
        // FORK(hollowed-oath): the character froze while this batch was in flight. Its reply has
        // removed what committed; drop the rest now.
        if (!CharacterComponent->IsFrozenAfterFinalRefusal()) {
          CharacterComponent->FreezeAfterFinalRefusal(TEXT("the character is frozen"));
        }
      } else if (CharacterComponent->IsCurrencyRebaseRequired()) {
        // A trade used the seq, or the reply could not confirm the balance: a reload must land
        // before anything is sent. Start it from this reply, and hand this attempt's settle to
        // it: for a detached pawn the entries are not saved yet, and releasing the write binding
        // now would refuse the resumed flush that saves them.
        if (Subsystem) {
          Subsystem->HoldItemFlushForCurrencyReload(
            CharacterId, WeakPlayerStateComponent.Get(), CharacterComponent, OnAckSettled
          );
          return;
        }
      }

      // Settle AFTER CompleteItemFlush, on both success and error: the release barrier's rule is
      // "the attempt resolved", which is exactly here.
      if (OnAckSettled) {
        OnAckSettled();
      }
    }
  );
}

// FORK(hollowed-oath): the currency ledger steps of the item flush. See the declarations.
void URedwoodServerGameSubsystem::ClaimCurrencyToSend(
  const URedwoodCharacterComponent *CharacterComponent,
  TArray<FRedwoodCurrencyEntry> &OutEntries,
  TArray<FString> &OutGrantKeys
) {
  const TArray<FRedwoodCurrencyEntry> &PendingEntries =
    CharacterComponent->GetPendingCurrencyEntries();
  const TArray<FString> &PendingGrantKeys = CharacterComponent->GetPendingGrantKeys();
  OutEntries.Append(
    PendingEntries.GetData(),
    FMath::Min(CharacterComponent->GetCurrencyEntriesToSendCount(), PendingEntries.Num())
  );
  OutGrantKeys.Append(
    PendingGrantKeys.GetData(),
    FMath::Min(CharacterComponent->GetGrantKeysToSendCount(), PendingGrantKeys.Num())
  );
}

FRedwoodItemFlushReply URedwoodServerGameSubsystem::ParseItemsFlushReply(
  const TSharedPtr<FJsonObject> &Reply
) {
  FRedwoodItemFlushReply Parsed;
  if (!Reply.IsValid()) {
    Parsed.Error = TEXT("unreadable item flush reply");
    return Parsed;
  }

  // A JSON null error reads as empty: that is the backend's success shape.
  Reply->TryGetStringField(TEXT("error"), Parsed.Error);
  // committedSeq rides both success and error (fence/replay) responses. TryGetNumberField leaves
  // the default when it is absent, and avoids the missing-field warning GetNumberField would log.
  // The default is -1, not 0: CompleteItemFlush reads a fence below the sent seq as a sequence gap
  // and realigns to it, and 0 is a real fence (a new character), so an absent field must not look
  // like one.
  double CommittedSeqValue = -1.0;
  Reply->TryGetNumberField(TEXT("committedSeq"), CommittedSeqValue);
  Parsed.CommittedSeq = static_cast<int64>(CommittedSeqValue);
  // Both optional: a backend that predates them sends neither, and both then mean false.
  Reply->TryGetBoolField(TEXT("final"), Parsed.bFinal);
  Reply->TryGetBoolField(TEXT("rebaseRequired"), Parsed.bRebaseRequired);
  Reply->TryGetBoolField(TEXT("notApplied"), Parsed.bNotApplied);
  Reply->TryGetBoolField(TEXT("writerFenceLost"), Parsed.bWriterFenceLost);
  ReadSidecarDeliveryFlags(
    Reply, Parsed.Error, FlushTimedOutError, Parsed.bTimedOut, Parsed.bNotForwarded
  );
  // The sidecar fills committedSeq with 0 on both. Read as a fence, that 0 is a sequence gap: the
  // lane would realign to 0, and a batch of item rows re-sent at seq 1 is acked as a replay and
  // never saved.
  // commitUnknown is the realm's own "may have applied": an unknown outcome, so it is no refusal and
  // no fence either, whatever else the reply says. The batch is then re-sent as a probe.
  Reply->TryGetBoolField(TEXT("commitUnknown"), Parsed.bCommitUnknown);
  if (Parsed.bCommitUnknown) {
    Parsed.bFinal = false;
    Parsed.bRebaseRequired = false;
    Parsed.bNotApplied = false;
    Parsed.bNotForwarded = false;
    if (Parsed.Error.IsEmpty()) {
      Parsed.Error = TEXT("the realm could not confirm the commit");
    }
  }
  if (Parsed.bTimedOut || Parsed.bNotForwarded || Parsed.bCommitUnknown) {
    Parsed.CommittedSeq = -1;
  }

  ParseCurrencyAlerts(Reply, Parsed.Alerts);
  return Parsed;
}

void URedwoodServerGameSubsystem::ReadSidecarDeliveryFlags(
  const TSharedPtr<FJsonObject> &Reply,
  const FString &Error,
  const TCHAR *TimedOutError,
  bool &OutTimedOut,
  bool &OutNotForwarded
) {
  OutTimedOut = false;
  OutNotForwarded = false;
  const bool bHasTimedOut = Reply->TryGetBoolField(TEXT("timedOut"), OutTimedOut);
  const bool bHasNotForwarded = Reply->TryGetBoolField(TEXT("notForwarded"), OutNotForwarded);
  if (bHasTimedOut || bHasNotForwarded || Error.IsEmpty()) {
    return;
  }
  if (Error.Equals(TimedOutError, ESearchCase::CaseSensitive)) {
    OutTimedOut = true;
  } else if (Error.Equals(SidecarRealmNotConnectedError, ESearchCase::CaseSensitive)) {
    OutNotForwarded = true;
  } else {
    return;
  }
  UE_LOG(
    LogRedwood,
    Warning,
    TEXT("Sidecar reply has no timedOut or notForwarded flag; read the outcome from its text: \"%s\". Update the sidecar."),
    *Error
  );
}

// FORK(hollowed-oath): a notApplied reply is never a commit, whatever its error text.
static bool IsItemsFlushReplyCommitted(const FRedwoodItemFlushReply &Reply) {
  return Reply.Error.IsEmpty() && !Reply.bNotApplied;
}

// A saved batch behind a later fence: the entries are committed, but the balance after them is
// not known (see WriteCommittedFlushToSnapshot).
static bool IsItemsFlushBalanceUnconfirmed(
  const FRedwoodItemFlushReply &Reply, const FRedwoodItemFlushSent &Sent
) {
  return IsItemsFlushReplyCommitted(Reply) && Sent.bHasBalanceAfter &&
    Reply.CommittedSeq > Sent.BatchSeq;
}

void URedwoodServerGameSubsystem::WriteCommittedFlushToSnapshot(
  FRedwoodCharacterBackend &Snapshot,
  const FRedwoodItemFlushReply &Reply,
  const FRedwoodItemFlushSent &Sent
) {
  const bool bCommitted = IsItemsFlushReplyCommitted(Reply);

  // Write the committed fence back onto the PlayerState snapshot. Only the COMPONENT's seq state
  // advances in CompleteItemFlush; the snapshot would otherwise stay at its login value. A
  // character component recreated later in the session -- a respawn re-creating a pawn-owned
  // component is the ordinary case -- seeds itself from that snapshot
  // (SeedItemSeqFromCharacter), so it would send an ALREADY USED batchSeq. The backend no-ops
  // that batch as a replay, and the no-op's success ack then clears the new component's dirty
  // rows: the mutation is lost with every side reporting success.
  if (Reply.Error.IsEmpty() && Reply.CommittedSeq > 0 &&
      Snapshot.InventorySeq < Reply.CommittedSeq) {
    Snapshot.InventorySeq = Reply.CommittedSeq;
  }
  // The committed balance, for the same reason: a recreated component and the game chain new
  // entries from it. Only when the fence is exactly this batch's seq: a later fence means
  // something else (a trade) committed after it, and the last BalanceAfter is then not the
  // balance. The snapshot then says so, and a reload sets it again.
  if (bCommitted && Sent.bHasBalanceAfter && Reply.CommittedSeq == Sent.BatchSeq) {
    Snapshot.CurrencyAmount = Sent.BalanceAfter;
    Snapshot.bCurrencyAmountUnknown = false;
  } else if (IsItemsFlushBalanceUnconfirmed(Reply, Sent)) {
    Snapshot.bCurrencyAmountUnknown = true;
  }

  // FORK(hollowed-oath): the committed grant keys, for the same reason: game code on a recreated
  // component checks the snapshot before it stages a one-time reward, and the backend refuses a
  // reused key for good. These are exactly the replies on which CompleteItemFlush drops the sent
  // keys as saved; a later fence still means this batch committed.
  if (bCommitted && Reply.CommittedSeq >= Sent.BatchSeq) {
    for (const FString &GrantKey : Sent.GrantKeys) {
      if (!ContainsGrantKey(Snapshot.GrantKeys, GrantKey)) {
        Snapshot.GrantKeys.Add(GrantKey);
      }
    }
  }
}

void URedwoodServerGameSubsystem::ApplyItemsFlushReply(
  URedwoodCharacterComponent *CharacterComponent,
  URedwoodPlayerStateComponent *PlayerStateComponent,
  const FRedwoodItemFlushReply &Reply,
  const FRedwoodItemFlushSent &Sent
) {
  const bool bBalanceUnconfirmed = IsItemsFlushBalanceUnconfirmed(Reply, Sent);

  // The snapshot is updated BEFORE CompleteItemFlush, because CompleteItemFlush runs a parked flush
  // attempt, and that attempt seeds its seq forward from the snapshot.
  if (PlayerStateComponent) {
    FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
    WriteCommittedFlushToSnapshot(Snapshot, Reply, Sent);

    // FORK(hollowed-oath): a notApplied reply also commits a seq (an empty one, that consumes the
    // probed seq), so the snapshot follows it for the same reason.
    if (Reply.bNotApplied && Reply.CommittedSeq >= Sent.BatchSeq &&
        Reply.CommittedSeq > Snapshot.InventorySeq) {
      Snapshot.InventorySeq = Reply.CommittedSeq;
    }

    // On a sequence gap the component realigns its lane back to the backend's fence. The snapshot
    // must follow, or the forward seed in FlushItemsForCharacterComponent moves the lane straight
    // back to the gapped seq, which the backend refuses forever.
    const bool bSequenceGap = !Reply.Error.IsEmpty() && !Reply.bFinal && !Reply.bRebaseRequired &&
      !Reply.bNotApplied &&
      Reply.CommittedSeq >= 0 && Reply.CommittedSeq < Sent.BatchSeq - 1;
    if (bSequenceGap && Snapshot.InventorySeq > Reply.CommittedSeq) {
      Snapshot.InventorySeq = Reply.CommittedSeq;
    }
  }

  // Also before CompleteItemFlush: the parked attempt it runs must already see the hold, or it
  // would send entries chained from a balance nobody has confirmed.
  if (bBalanceUnconfirmed) {
    CharacterComponent->RequireCurrencyReload(true);
  }

  CharacterComponent->CompleteItemFlush(
    Reply.Error,
    Reply.CommittedSeq,
    Sent.Upserts,
    Sent.Deletes,
    Sent.CurrencyCount,
    Sent.GrantKeys.Num(),
    Reply.bFinal,
    Reply.bRebaseRequired,
    Reply.bNotApplied,
    Reply.bNotForwarded
  );

  if (Reply.Alerts.Num() > 0) {
    CharacterComponent->OnCurrencyAlerts.Broadcast(Reply.Alerts);
  }
}

ERedwoodCurrencyReloadResult URedwoodServerGameSubsystem::ParseCurrencyReloadReply(
  const TSharedPtr<FJsonObject> &Reply, int64 &OutBalance, TArray<FString> &OutGrantKeys
) {
  // Every error reply also carries a filler balance of "0", so the error is checked first: a
  // rebase from that filler would chain every held entry from zero.
  if (!Reply.IsValid() || !Reply->HasField(TEXT("error"))) {
    UE_LOG(LogRedwood, Error, TEXT("Currency reload failed: unreadable reply"));
    return ERedwoodCurrencyReloadResult::Retry;
  }
  // A JSON null error reads as empty: that is the backend's success shape.
  FString Error;
  Reply->TryGetStringField(TEXT("error"), Error);
  // Only the machine-readable flag ends the reload: another session owns the character, and no
  // retry changes that. A "not bound" error without it is retriable, since the instance binding
  // comes back (after a sidecar restart, for one).
  bool bWriterFenceLost = false;
  Reply->TryGetBoolField(TEXT("writerFenceLost"), bWriterFenceLost);
  if (!Error.IsEmpty() && bWriterFenceLost) {
    UE_LOG(LogRedwood, Error, TEXT("Currency reload failed for good: %s"), *Error);
    return ERedwoodCurrencyReloadResult::GiveUp;
  }
  if (!Error.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Currency reload failed: %s"), *Error);
    return ERedwoodCurrencyReloadResult::Retry;
  }

  // The same rule as the auth leg's load (TryParseCurrencyLoad): the grant keys are part of the
  // loaded state, and a reload that dropped them would leave the snapshot's keys stale.
  const TSharedPtr<FJsonObject> *Currency = nullptr;
  if (!Reply->TryGetObjectField(TEXT("currency"), Currency) || !Currency ||
      !TryParseCurrencyLoad(*Currency, OutBalance, OutGrantKeys)) {
    UE_LOG(LogRedwood, Error, TEXT("Currency reload failed: the reply has no readable currency"));
    return ERedwoodCurrencyReloadResult::Retry;
  }
  return ERedwoodCurrencyReloadResult::Applied;
}

bool URedwoodServerGameSubsystem::ApplyCurrencyReload(
  URedwoodCharacterComponent *CharacterComponent,
  URedwoodPlayerStateComponent *PlayerStateComponent,
  int64 Balance,
  const TArray<FString> &GrantKeys
) {
  if (CharacterComponent->IsCurrencyRebaseRequired()) {
    CharacterComponent->RebaseCurrency(Balance);
    if (CharacterComponent->IsCurrencyRebaseRequired()) {
      // RebaseCurrency logged why (an overflow). The entries stay held and unchanged.
      UE_LOG(
        LogRedwood, Error, TEXT("Currency reload failed: the rebase from %lld did not apply"), Balance
      );
      return false;
    }
  }

  // The loaded balance is committed, so the snapshot takes it, as it takes a flush's. The keys are
  // merged in, not swapped in: the backend never removes a key, so every key on the snapshot is
  // still used, and one that a racing batch committed after the reload read the database would be
  // lost by a replace. The merge adds a key that a flush committed but whose ack was lost.
  if (PlayerStateComponent) {
    FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
    Snapshot.CurrencyAmount = Balance;
    Snapshot.bCurrencyAmountUnknown = false;
    for (const FString &GrantKey : GrantKeys) {
      if (!ContainsGrantKey(Snapshot.GrantKeys, GrantKey)) {
        Snapshot.GrantKeys.Add(GrantKey);
      }
    }
  }

  CharacterComponent->OnCurrencyRebased.Broadcast(Balance);
  return true;
}

void URedwoodServerGameSubsystem::FreezeCharacter(
  const FString &CharacterId, URedwoodPlayerStateComponent *PlayerStateComponent
) {
  FrozenCharacterIds.Add(CharacterId);
  // A component created later from this snapshot (a respawn) starts frozen too.
  if (PlayerStateComponent) {
    PlayerStateComponent->RedwoodCharacter.bFrozenAfterFinalRefusal = true;
  }
  // A rebase is pointless now. Its held attempts settle; each held component drops its staged
  // state on its next flush.
  if (CurrencyReloads.Contains(CharacterId)) {
    GiveUpCurrencyReload(CharacterId, TEXT("the character is frozen after a final refusal"));
  }
}

void URedwoodServerGameSubsystem::HoldItemFlushForCurrencyReload(
  const FString &CharacterId,
  URedwoodPlayerStateComponent *PlayerStateComponent,
  URedwoodCharacterComponent *CharacterComponent,
  TFunction<void()> OnAckSettled
) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  const bool bStart = Reload == nullptr;
  if (bStart) {
    Reload = &CurrencyReloads.Add(CharacterId);
  }
  if (PlayerStateComponent) {
    Reload->PlayerStateComponent = PlayerStateComponent;
  }
  if (CharacterComponent->IsCurrencyRebaseRequired()) {
    Reload->CharacterComponents.AddUnique(CharacterComponent);
  }
  AddHeldItemFlush(
    Reload->HeldFlushes, PlayerStateComponent, CharacterComponent, MoveTemp(OnAckSettled)
  );

  if (bStart) {
    SendCurrencyReload(CharacterId);
  }
}

void URedwoodServerGameSubsystem::SendCurrencyReload(const FString &CharacterId) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload) {
    return;
  }

  // The token is the session's, the same on every component of the character.
  FString WriterToken;
  bool bAnyComponent = false;
  for (const TWeakObjectPtr<URedwoodCharacterComponent> &WeakComponent : Reload->CharacterComponents) {
    if (const URedwoodCharacterComponent *Component = WeakComponent.Get()) {
      bAnyComponent = true;
      if (WriterToken.IsEmpty()) {
        WriterToken = Component->GetWriterToken();
      }
    }
  }
  if (!bAnyComponent) {
    // Every component that needed the reload is gone, and its held currency with it. The held
    // attempts of the others settle; their next flush goes out as usual.
    AbandonCurrencyReload(CharacterId);
    return;
  }

  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    // Waiting for the sidecar is not a failed load: the connect event sends it again, and no
    // attempt is spent, so a reconnect cannot run a detached pawn out of attempts.
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Currency reload for character %s waits for the sidecar to connect"),
      *CharacterId
    );
    Reload->bWaitingForSidecar = true;
    return;
  }
  Reload->bWaitingForSidecar = false;
  ++Reload->Attempts;

  // The load carries the session's writer token, so the realm refuses it once another session owns
  // the character, instead of handing out a balance this session can no longer write. Without a
  // token this session's flushes are refused too, so that is a failed load, retried like the rest
  // (the token is seeded at hydration).
  if (WriterToken.IsEmpty()) {
    UE_LOG(
      LogRedwood, Error, TEXT("Currency reload failed for character %s: no writer token"), *CharacterId
    );
    RetryCurrencyReload(CharacterId);
    return;
  }

  UGameInstance *GameInstance = GetGameInstance();
  if (!GameInstance) {
    GiveUpCurrencyReload(CharacterId, TEXT("no game instance to time the reply"));
    return;
  }

  const uint64 Generation = ++LastCurrencyReloadGeneration;
  Reload->Generation = Generation;

  // A reply that never comes (the socket dropped) must not hold the flushes forever.
  GameInstance->GetTimerManager().SetTimer(
    Reload->RetryTimer,
    FTimerDelegate::CreateWeakLambda(
      this,
      [this, CharacterId, Generation]() { HandleCurrencyReloadTimeout(CharacterId, Generation); }
    ),
    CurrencyReloadReplyTimeoutSeconds,
    false // no loop
  );

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("id"), ItemsEnvelopePlaceholderId);
  Payload->SetStringField(TEXT("characterId"), CharacterId);
  Payload->SetStringField(TEXT("writerToken"), WriterToken);

  TWeakObjectPtr<URedwoodServerGameSubsystem> WeakSubsystem(this);
  Sidecar->Emit(
    TEXT("realm:characters:items:load"),
    Payload,
    [WeakSubsystem, CharacterId, Generation](auto Response) {
      URedwoodServerGameSubsystem *Subsystem = WeakSubsystem.Get();
      if (!Subsystem) {
        return;
      }
      const TSharedPtr<FJsonObject> *ReplyObject =
        URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
      Subsystem->HandleCurrencyReloadReply(
        CharacterId, Generation, ReplyObject ? *ReplyObject : TSharedPtr<FJsonObject>()
      );
    }
  );
}

void URedwoodServerGameSubsystem::HandleCurrencyReloadReply(
  const FString &CharacterId, uint64 Generation, const TSharedPtr<FJsonObject> &Reply
) {
  // A late reply to an attempt that timed out, or to a reload that already ended, may carry a
  // balance from before a trade that the current attempt's balance includes.
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload || Reload->Generation != Generation) {
    UE_LOG(
      LogRedwood,
      Log,
      TEXT("Ignoring a late currency reload reply for character %s"),
      *CharacterId
    );
    return;
  }
  Reload->Generation = 0;
  if (UGameInstance *GameInstance = GetGameInstance()) {
    GameInstance->GetTimerManager().ClearTimer(Reload->RetryTimer);
  }

  int64 Balance = 0;
  TArray<FString> GrantKeys;
  switch (ParseCurrencyReloadReply(Reply, Balance, GrantKeys)) {
    case ERedwoodCurrencyReloadResult::GiveUp:
      GiveUpCurrencyReload(CharacterId, TEXT("this session no longer owns the character"));
      return;
    case ERedwoodCurrencyReloadResult::Retry:
      RetryCurrencyReload(CharacterId);
      return;
    default:
      break;
  }

  // Copies: ApplyCurrencyReload tells the game, and the game may flush (and so hold) in response,
  // which changes the map and this reload's arrays.
  const TArray<TWeakObjectPtr<URedwoodCharacterComponent>> Components = Reload->CharacterComponents;
  const TWeakObjectPtr<URedwoodPlayerStateComponent> WeakPlayerState = Reload->PlayerStateComponent;
  for (const TWeakObjectPtr<URedwoodCharacterComponent> &WeakComponent : Components) {
    URedwoodCharacterComponent *Component = WeakComponent.Get();
    if (IsValid(Component) && Component->IsCurrencyRebaseRequired() &&
        !ApplyCurrencyReload(Component, WeakPlayerState.Get(), Balance, GrantKeys)) {
      RetryCurrencyReload(CharacterId);
      return;
    }
  }
  Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload) {
    return;
  }

  // Out of the map before any flush resumes: a resumed flush can need a reload again (a trade
  // used the new seq too) and must then start a new one, not join this finished one.
  FRedwoodCurrencyReload Finished = MoveTemp(*Reload);
  CurrencyReloads.Remove(CharacterId);
  if (UGameInstance *GameInstance = GetGameInstance()) {
    GameInstance->GetTimerManager().ClearTimer(Finished.RetryTimer);
  }

  ResumeHeldItemFlushes(Finished.HeldFlushes);
}

void URedwoodServerGameSubsystem::HandleCurrencyReloadTimeout(
  const FString &CharacterId, uint64 Generation
) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload || Reload->Generation != Generation) {
    return;
  }
  // From here a reply to this attempt is late, and is ignored.
  Reload->Generation = 0;
  UE_LOG(
    LogRedwood, Error, TEXT("Currency reload failed for character %s: no reply"), *CharacterId
  );
  RetryCurrencyReload(CharacterId);
}

void URedwoodServerGameSubsystem::RetryCurrencyReload(const FString &CharacterId) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload) {
    return;
  }
  if (Reload->Attempts >= CurrencyReloadMaxAttempts) {
    GiveUpCurrencyReload(
      CharacterId, FString::Printf(TEXT("%d attempts failed"), Reload->Attempts)
    );
    return;
  }
  UGameInstance *GameInstance = GetGameInstance();
  if (!GameInstance) {
    GiveUpCurrencyReload(CharacterId, TEXT("no game instance to time the retry"));
    return;
  }
  GameInstance->GetTimerManager().SetTimer(
    Reload->RetryTimer,
    FTimerDelegate::CreateWeakLambda(
      this, [this, CharacterId]() { SendCurrencyReload(CharacterId); }
    ),
    CurrencyReloadRetrySeconds,
    false // no loop
  );
}

void URedwoodServerGameSubsystem::ResumeCurrencyReloadsAfterSidecarConnect() {
  // Keys first: a send can end its reload and change the map.
  TArray<FString> WaitingCharacterIds;
  for (const TPair<FString, FRedwoodCurrencyReload> &Pair : CurrencyReloads) {
    if (Pair.Value.bWaitingForSidecar) {
      WaitingCharacterIds.Add(Pair.Key);
    }
  }
  for (const FString &CharacterId : WaitingCharacterIds) {
    SendCurrencyReload(CharacterId);
  }
}

void URedwoodServerGameSubsystem::GiveUpCurrencyReload(
  const FString &CharacterId, const FString &Reason
) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload) {
    return;
  }

  // Everything still held. A live component keeps it and tries again on its next flush; a
  // detached pawn's is lost when the pawn goes, so this line is what staff rebuild it from.
  TArray<FString> HeldLines;
  for (const TWeakObjectPtr<URedwoodCharacterComponent> &WeakComponent : Reload->CharacterComponents) {
    const URedwoodCharacterComponent *Component = WeakComponent.Get();
    if (!Component) {
      continue;
    }
    for (const FRedwoodCurrencyEntry &Entry : Component->GetPendingCurrencyEntries()) {
      HeldLines.Add(FString::Printf(
        TEXT("entry %s delta %lld reason %s"), *Entry.Id, Entry.Delta, *Entry.Reason
      ));
    }
    for (const FString &GrantKey : Component->GetPendingGrantKeys()) {
      HeldLines.Add(FString::Printf(TEXT("grant %s"), *GrantKey));
    }
  }
  UE_LOG(
    LogRedwood,
    Error,
    TEXT(
      "Currency reload for character %s gave up (%s). Its held flushes now settle; these are NOT saved: [%s]"
    ),
    *CharacterId,
    *Reason,
    *FString::Join(HeldLines, TEXT("; "))
  );
  AbandonCurrencyReload(CharacterId);
}

void URedwoodServerGameSubsystem::AbandonCurrencyReload(const FString &CharacterId) {
  FRedwoodCurrencyReload *Reload = CurrencyReloads.Find(CharacterId);
  if (!Reload) {
    return;
  }
  if (UGameInstance *GameInstance = GetGameInstance()) {
    GameInstance->GetTimerManager().ClearTimer(Reload->RetryTimer);
  }
  // Out of the map first: a settle can release a binding and start other work that reaches here.
  FRedwoodCurrencyReload Abandoned = MoveTemp(*Reload);
  CurrencyReloads.Remove(CharacterId);
  for (FRedwoodHeldItemFlush &Held : Abandoned.HeldFlushes) {
    if (Held.OnAckSettled) {
      Held.OnAckSettled();
    }
  }
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

// FORK(hollowed-oath) BEGIN: the offline currency save. See the declaration.
bool URedwoodServerGameSubsystem::ApplyOfflineCurrency(
  FRedwoodCharacterBackend &Character, URedwoodCharacterComponent *CharacterComponent
) {
  const int32 StagedEntryCount = CharacterComponent->GetPendingCurrencyEntries().Num();
  const int32 StagedGrantCount = CharacterComponent->GetPendingGrantKeys().Num();
  if (StagedEntryCount == 0 && StagedGrantCount == 0) {
    return false;
  }
  if (Character.bCurrencyAmountUnknown) {
    // Chaining from a guessed balance would write a wrong one. The entries stay staged, and the
    // file keeps the balance it has.
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Offline save: character %s has an unknown balance; %d currency entries and %d grant keys stay staged"),
      *Character.Id,
      StagedEntryCount,
      StagedGrantCount
    );
    return false;
  }

  TArray<FRedwoodCurrencyEntry> Entries;
  TArray<FString> GrantKeys;
  CharacterComponent->TakePendingCurrencyForOfflineSave(Entries, GrantKeys);

  // The backend's flush rules, so offline play finds the same caller bugs a live server would.
  FString Refusal;
  int64 Balance = Character.CurrencyAmount;
  // Ids are checked uppercase hex at the queue, so a plain (case-blind) compare is exact here.
  TArray<FString> AppliedEntryIds = Character.OfflineAppliedEntryIds;
  for (const FRedwoodCurrencyEntry &Entry : Entries) {
    if (Entry.Reason.IsEmpty()) {
      Refusal = FString::Printf(TEXT("entry %s has no reason"), *Entry.Id);
      break;
    }
    if (AppliedEntryIds.Contains(Entry.Id)) {
      Refusal = FString::Printf(TEXT("entry %s is already applied"), *Entry.Id);
      break;
    }
    AppliedEntryIds.Add(Entry.Id);
    // Balance is never negative here, so only a credit can pass int64.
    if (Entry.Delta > 0 && Balance > MAX_int64 - Entry.Delta) {
      Refusal = FString::Printf(TEXT("entry %s takes the balance past int64"), *Entry.Id);
      break;
    }
    Balance += Entry.Delta;
    if (Balance < 0) {
      Refusal = FString::Printf(TEXT("entry %s takes the balance below zero"), *Entry.Id);
      break;
    }
    if (Balance != Entry.BalanceAfter) {
      Refusal = FString::Printf(
        TEXT("entry %s ends at %lld, not at its balanceAfter %lld"),
        *Entry.Id,
        Balance,
        Entry.BalanceAfter
      );
      break;
    }
  }
  TArray<FString> UsedGrantKeys = Character.GrantKeys;
  for (const FString &GrantKey : GrantKeys) {
    if (Refusal.IsEmpty() && ContainsGrantKey(UsedGrantKeys, GrantKey)) {
      Refusal = FString::Printf(TEXT("grant key %s is already used"), *GrantKey);
    }
    UsedGrantKeys.Add(GrantKey);
  }

  if (!Refusal.IsEmpty()) {
    // As a final refusal online: the whole batch is dropped, nothing moves, and each part is
    // logged so it can be rebuilt by hand.
    for (const FRedwoodCurrencyEntry &Entry : Entries) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Offline save refused (%s): character %s dropped currency entry %s delta %lld reason %s, NOT saved"),
        *Refusal,
        *Character.Id,
        *Entry.Id,
        Entry.Delta,
        *Entry.Reason
      );
    }
    for (const FString &GrantKey : GrantKeys) {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Offline save refused (%s): character %s dropped grant key %s, NOT saved"),
        *Refusal,
        *Character.Id,
        *GrantKey
      );
    }
    return false;
  }

  Character.CurrencyAmount = Balance;
  Character.GrantKeys.Append(GrantKeys);
  Character.OfflineAppliedEntryIds = MoveTemp(AppliedEntryIds);
  return true;
}
// FORK(hollowed-oath) END

// One-shot trade emit: atomically re-parents the named root items from one character to another on
// the backend. rootPlacements carries only each root row's (id, domain, slot, parentId); the
// backend moves each root and its nested children in one transaction and bumps BOTH characters'
// InventorySeq. parentId is optional on the wire (absent/null = root placement into the receiving
// character's top-level inventory); non-null names the receiver-side bag item's InstanceId a
// content-child placement lands inside -- production trades move items between bags, so a
// zero-value (root-only) placement can no longer represent every destination a trade needs.
// FORK(hollowed-oath): the trade carries currency too, and holds both characters' flush lanes
// until it settles (see the header). The rules each step keeps are on the step.
void URedwoodServerGameSubsystem::EmitItemsTrade(
  URedwoodCharacterComponent *FromCharacterComponent,
  URedwoodCharacterComponent *ToCharacterComponent,
  const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
  const FString &TradeId,
  TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete
) {
  // Every refusal here is before the send: nothing is held, staged or changed, so the caller can
  // treat it like any trade error.
  auto Refuse = [&OnComplete, &TradeId](const FString &Error) {
    UE_LOG(LogRedwood, Error, TEXT("Trade %s refused before sending: %s"), *TradeId, *Error);
    if (OnComplete) {
      OnComplete(Error, -1, -1);
    }
  };

  FRedwoodPendingItemTrade Trade;
  FString Error;
  if (!PrepareItemTradeSide(FromCharacterComponent, Trade.From, Error) ||
      !PrepareItemTradeSide(ToCharacterComponent, Trade.To, Error)) {
    Refuse(Error);
    return;
  }
  if (Trade.From.CharacterId == Trade.To.CharacterId) {
    Refuse(TEXT("cannot trade to the same character"));
    return;
  }
  // The backend needs the id to know a resend, and refuses any currency without it.
  if (!IsValidCurrencyEntryId(TradeId)) {
    Refuse(TEXT("the trade id must be 32 uppercase hex digits"));
    return;
  }
  // A reused id is never a resend here (the subsystem resends by itself): it is a second trade,
  // which the backend would take for the first one's replay, or refuse.
  if (SentItemTradeIds.Contains(TradeId)) {
    Refuse(TEXT("the trade id was already used; make a fresh one for every trade"));
    return;
  }
  for (const FRedwoodTradeRootPlacement &Placement : RootPlacements) {
    if (!Placement.FromCharacterId.IsEmpty() && Placement.FromCharacterId != Trade.From.CharacterId &&
        Placement.FromCharacterId != Trade.To.CharacterId) {
      Refuse(FString::Printf(
        TEXT("placement %s leaves character %s, who is outside the trade"),
        *Placement.Id,
        *Placement.FromCharacterId
      ));
      return;
    }
  }
  if (!ChainTradeCurrency(CurrencyByCharacter, Trade.From, Trade.To, Error)) {
    Refuse(Error);
    return;
  }
  // A side that pays or is paid must have nothing staged: the trade commits before it, so a staged
  // debit chained from the old balance could fall below zero once the trade applied, and that is
  // refused for good.
  for (const FRedwoodItemTradeSide *Side : {&Trade.From, &Trade.To}) {
    if (Side->Entries.Num() > 0 &&
        Side->CharacterComponent->GetPendingCurrencyEntries().Num() > 0) {
      Refuse(FString::Printf(
        TEXT("character %s has currency staged; flush it before the trade"), *Side->CharacterId
      ));
      return;
    }
  }
  if (Sidecar == nullptr || !Sidecar.IsValid() || !Sidecar->bIsConnected) {
    Refuse(TEXT("Sidecar is not connected"));
    return;
  }

  // The payload goes out with the chained entries, exactly as the sides hold them, so a resend is
  // the same request.
  TArray<TPair<FString, FRedwoodCurrencyEntry>> ChainedCurrency;
  for (const FRedwoodItemTradeSide *Side : {&Trade.From, &Trade.To}) {
    for (const FRedwoodCurrencyEntry &Entry : Side->Entries) {
      ChainedCurrency.Emplace(Side->CharacterId, Entry);
    }
  }
  Trade.Payload = BuildItemsTradePayload(
    Trade.From.CharacterId,
    Trade.To.CharacterId,
    RootPlacements,
    ChainedCurrency,
    Trade.From.CharacterComponent->GetWriterToken(),
    Trade.To.CharacterComponent->GetWriterToken(),
    TradeId
  );
  Trade.OnComplete = MoveTemp(OnComplete);
  SentItemTradeIds.Add(TradeId);
  PendingItemTrades.Add(TradeId, MoveTemp(Trade));
  SendItemsTrade(TradeId);
}

void URedwoodServerGameSubsystem::EmitItemsTrade(
  const FString &FromCharacterId,
  const FString &ToCharacterId,
  const TArray<FRedwoodTradeRootPlacement> &RootPlacements,
  TFunction<void(FString Error, int64 FromCommittedSeq, int64 ToCommittedSeq)> OnComplete
) {
  const FString Error = TEXT(
    "this trade call sends no writer tokens and cannot hold the flush lanes; use the overload that "
    "takes the character components"
  );
  UE_LOG(
    LogRedwood, Error, TEXT("Trade %s -> %s refused: %s"), *FromCharacterId, *ToCharacterId, *Error
  );
  if (OnComplete) {
    OnComplete(Error, -1, -1);
  }
}

bool URedwoodServerGameSubsystem::PrepareItemTradeSide(
  URedwoodCharacterComponent *CharacterComponent,
  FRedwoodItemTradeSide &OutSide,
  FString &OutError
) {
  if (!IsValid(CharacterComponent) || CharacterComponent->RedwoodCharacterId.IsEmpty()) {
    OutError = TEXT("a trading character has no hydrated character component");
    return false;
  }
  const FString &CharacterId = CharacterComponent->RedwoodCharacterId;
  // The snapshot holds the committed balance the entries chain from, and takes the trade's commit.
  URedwoodPlayerStateComponent *PlayerStateComponent =
    CharacterComponent->FindPlayerStateComponent();
  if (!PlayerStateComponent || PlayerStateComponent->RedwoodCharacter.Id != CharacterId) {
    OutError = FString::Printf(TEXT("character %s has no PlayerState snapshot"), *CharacterId);
    return false;
  }
  // A frozen character's state cannot be trusted, and a lost fence means another session owns it:
  // neither may move items or currency.
  if (CharacterComponent->IsFrozenAfterFinalRefusal() || FrozenCharacterIds.Contains(CharacterId)) {
    OutError = FString::Printf(TEXT("character %s is frozen after a final refusal"), *CharacterId);
    return false;
  }
  if (CharacterComponent->IsWriterFenceLost() || CharacterComponent->GetWriterToken().IsEmpty()) {
    OutError = FString::Printf(TEXT("character %s has no writer fence"), *CharacterId);
    return false;
  }
  if (FindItemTradeOfCharacter(CharacterId)) {
    OutError = FString::Printf(TEXT("character %s is in another trade"), *CharacterId);
    return false;
  }
  // A batch whose reply is open, or whose outcome is unknown, would commit before or after the
  // trade: the balance and the seq before the trade would then be guesses.
  if (CharacterComponent->IsItemFlushInFlight() || CharacterComponent->IsCurrencyBatchUnconfirmed() ||
      InFlightItemFlushesByCharacter.Contains(CharacterId) ||
      HasUnconfirmedCurrencyBatch(CharacterId)) {
    OutError = FString::Printf(TEXT("character %s has an item flush in flight or unconfirmed"), *CharacterId);
    return false;
  }

  CatchUpItemSeqFromSnapshot(PlayerStateComponent, CharacterComponent);
  const FRedwoodCharacterBackend &Snapshot = PlayerStateComponent->RedwoodCharacter;
  if (CharacterComponent->IsCurrencyRebaseRequired() || CurrencyReloads.Contains(CharacterId)) {
    OutError = FString::Printf(TEXT("character %s waits for a currency reload"), *CharacterId);
    return false;
  }

  OutSide.CharacterId = CharacterId;
  OutSide.CharacterComponent = CharacterComponent;
  OutSide.PlayerStateComponent = PlayerStateComponent;
  OutSide.SeqBefore = CharacterComponent->GetLastCommittedItemSeq();
  // Only read when the side pays or is paid; ChainTradeCurrency refuses an unknown balance then.
  OutSide.BalanceBefore = Snapshot.bCurrencyAmountUnknown ? -1 : Snapshot.CurrencyAmount;
  return true;
}

bool URedwoodServerGameSubsystem::ChainTradeCurrency(
  const TArray<TPair<FString, FRedwoodCurrencyEntry>> &CurrencyByCharacter,
  FRedwoodItemTradeSide &From,
  FRedwoodItemTradeSide &To,
  FString &OutError
) {
  // Each check here is one the backend makes too; refusing first spares both players a refusal
  // alert for a trade that could never apply.
  TArray<FString> SeenIds;
  int64 Sum = 0;
  int64 FromRunning = From.BalanceBefore;
  int64 ToRunning = To.BalanceBefore;
  TArray<FRedwoodCurrencyEntry> FromEntries;
  TArray<FRedwoodCurrencyEntry> ToEntries;
  for (const TPair<FString, FRedwoodCurrencyEntry> &Pair : CurrencyByCharacter) {
    const FRedwoodCurrencyEntry &Entry = Pair.Value;
    const bool bFrom = Pair.Key == From.CharacterId;
    if (!bFrom && Pair.Key != To.CharacterId) {
      OutError = FString::Printf(TEXT("currency entry %s is for a character outside the trade"), *Entry.Id);
      return false;
    }
    if (!IsValidCurrencyEntryId(Entry.Id) ||
        SeenIds.ContainsByPredicate([&Entry](const FString &Seen) {
          return Seen.Equals(Entry.Id, ESearchCase::CaseSensitive);
        })) {
      OutError = FString::Printf(TEXT("currency entry id '%s' is not valid or is repeated"), *Entry.Id);
      return false;
    }
    SeenIds.Add(Entry.Id);
    if (!Entry.Reason.Equals(TradeCurrencyReason, ESearchCase::CaseSensitive)) {
      OutError = FString::Printf(
        TEXT("currency entry %s has reason %s, not %s"), *Entry.Id, *Entry.Reason, TradeCurrencyReason
      );
      return false;
    }
    if (Entry.Delta == 0) {
      OutError = FString::Printf(TEXT("currency entry %s has a zero delta"), *Entry.Id);
      return false;
    }
    // BalanceBefore is -1 when the snapshot's balance is unknown: nothing can be chained from it.
    int64 &Running = bFrom ? FromRunning : ToRunning;
    if (Running < 0) {
      OutError = FString::Printf(TEXT("the balance of character %s is not known"), *Pair.Key);
      return false;
    }
    const bool bOverflows = Entry.Delta > 0 ? Running > MAX_int64 - Entry.Delta : false;
    const bool bSumOverflows = Entry.Delta > 0 ? Sum > MAX_int64 - Entry.Delta
                                               : Sum < MIN_int64 - Entry.Delta;
    if (bOverflows || bSumOverflows) {
      OutError = FString::Printf(TEXT("currency entry %s overflows"), *Entry.Id);
      return false;
    }
    // Running >= 0 and a negative delta cannot overflow; it can only go below zero.
    Running += Entry.Delta;
    Sum += Entry.Delta;
    if (Running < 0) {
      OutError = FString::Printf(TEXT("character %s cannot pay currency entry %s"), *Pair.Key, *Entry.Id);
      return false;
    }
    FRedwoodCurrencyEntry &Chained = (bFrom ? FromEntries : ToEntries).Add_GetRef(Entry);
    Chained.BalanceAfter = Running;
  }
  if (Sum != 0) {
    OutError = FString::Printf(TEXT("trade currency does not sum to zero (%lld)"), Sum);
    return false;
  }
  From.Entries = MoveTemp(FromEntries);
  To.Entries = MoveTemp(ToEntries);
  return true;
}

bool URedwoodServerGameSubsystem::HasUnconfirmedCurrencyBatch(const FString &CharacterId) {
  TArray<TWeakObjectPtr<URedwoodCharacterComponent>> *Components =
    UnconfirmedCurrencyComponentsByCharacter.Find(CharacterId);
  if (!Components) {
    return false;
  }
  // Read live: a later reply, a freeze or a destroyed component ends the unknown outcome without
  // telling this list.
  Components->RemoveAll([](const TWeakObjectPtr<URedwoodCharacterComponent> &Weak) {
    const URedwoodCharacterComponent *Component = Weak.Get();
    return !IsValid(Component) || !Component->IsCurrencyBatchUnconfirmed();
  });
  if (Components->Num() == 0) {
    UnconfirmedCurrencyComponentsByCharacter.Remove(CharacterId);
    return false;
  }
  return true;
}

void URedwoodServerGameSubsystem::EndItemFlushInFlight(const FString &CharacterId) {
  int32 *Count = InFlightItemFlushesByCharacter.Find(CharacterId);
  if (!Count) {
    return;
  }
  // Removed at zero, so Contains means "a batch is open".
  if (--*Count <= 0) {
    InFlightItemFlushesByCharacter.Remove(CharacterId);
  }
}

bool URedwoodServerGameSubsystem::GetUnsettledTradeFloorBalance(
  const FString &CharacterId, int64 &OutFloorBalance
) const {
  for (const TPair<FString, FRedwoodPendingItemTrade> &Pair : PendingItemTrades) {
    for (const FRedwoodItemTradeSide *Side : {&Pair.Value.From, &Pair.Value.To}) {
      if (Side->CharacterId == CharacterId && Side->Entries.Num() > 0) {
        OutFloorBalance = FMath::Min(Side->BalanceBefore, Side->Entries.Last().BalanceAfter);
        return true;
      }
    }
  }
  return false;
}

FRedwoodPendingItemTrade *URedwoodServerGameSubsystem::FindItemTradeOfCharacter(
  const FString &CharacterId
) {
  for (TPair<FString, FRedwoodPendingItemTrade> &Pair : PendingItemTrades) {
    if (Pair.Value.From.CharacterId == CharacterId || Pair.Value.To.CharacterId == CharacterId) {
      return &Pair.Value;
    }
  }
  return nullptr;
}

void URedwoodServerGameSubsystem::SendItemsTrade(const FString &TradeId) {
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade) {
    return;
  }
  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    // Only a resend gets here (the first send is refused instead). Waiting spends no attempt, so a
    // reconnect cannot run the trade out of attempts; the lanes stay held meanwhile.
    UE_LOG(LogRedwood, Warning, TEXT("Trade %s waits for the sidecar to connect"), *TradeId);
    Trade->bWaitingForSidecar = true;
    return;
  }
  Trade->bWaitingForSidecar = false;

  UGameInstance *GameInstance = GetGameInstance();
  if (!GameInstance) {
    GiveUpItemsTrade(TradeId, TEXT("no game instance to time the reply"));
    return;
  }
  ++Trade->Attempts;
  const uint64 Generation = ++LastItemTradeGeneration;
  Trade->Generation = Generation;

  // A reply that never comes (the socket dropped) must not hold both lanes forever.
  GameInstance->GetTimerManager().SetTimer(
    Trade->Timer,
    FTimerDelegate::CreateWeakLambda(
      this, [this, TradeId, Generation]() { HandleItemsTradeTimeout(TradeId, Generation); }
    ),
    TradeReplyTimeoutSeconds,
    false // no loop
  );

  TWeakObjectPtr<URedwoodServerGameSubsystem> WeakSubsystem(this);
  Sidecar->Emit(
    TEXT("realm:characters:items:trade"),
    Trade->Payload,
    [WeakSubsystem, TradeId, Generation](auto Response) {
      URedwoodServerGameSubsystem *Subsystem = WeakSubsystem.Get();
      if (!Subsystem) {
        return;
      }
      const TSharedPtr<FJsonObject> *ReplyObject =
        URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
      Subsystem->HandleItemsTradeReply(
        TradeId, Generation, ReplyObject ? *ReplyObject : TSharedPtr<FJsonObject>()
      );
    }
  );
}

FRedwoodItemTradeReply URedwoodServerGameSubsystem::ParseItemsTradeReply(
  const TSharedPtr<FJsonObject> &Reply
) {
  FRedwoodItemTradeReply Parsed;
  if (!Reply.IsValid() || !Reply->HasField(TEXT("error"))) {
    Parsed.Error = TEXT("unreadable item trade reply");
    return Parsed;
  }
  Parsed.bReadable = true;
  // A JSON null error reads as empty: that is the backend's success shape.
  Reply->TryGetStringField(TEXT("error"), Parsed.Error);
  // JSON numbers land as doubles, and TryGetNumberField has no int64 overload here. A missing
  // fence stays -1, which no committed trade carries.
  double FromCommittedSeqValue = -1.0;
  double ToCommittedSeqValue = -1.0;
  Reply->TryGetNumberField(TEXT("fromCommittedSeq"), FromCommittedSeqValue);
  Reply->TryGetNumberField(TEXT("toCommittedSeq"), ToCommittedSeqValue);
  Parsed.FromCommittedSeq = static_cast<int64>(FromCommittedSeqValue);
  Parsed.ToCommittedSeq = static_cast<int64>(ToCommittedSeqValue);
  ReadSidecarDeliveryFlags(
    Reply, Parsed.Error, TradeTimedOutError, Parsed.bTimedOut, Parsed.bNotForwarded
  );
  Reply->TryGetBoolField(TEXT("commitUnknown"), Parsed.bCommitUnknown);
  if (!Reply->TryGetBoolField(TEXT("sessionRefused"), Parsed.bSessionRefused) &&
      (Parsed.Error.Equals(TradeNotBoundError, ESearchCase::CaseSensitive) ||
       Parsed.Error.Equals(TradeNotAuthorizedError, ESearchCase::CaseSensitive))) {
    Parsed.bSessionRefused = true;
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Trade reply has no sessionRefused flag; read the outcome from its text: \"%s\". Update the realm."),
      *Parsed.Error
    );
  }
  ParseCurrencyAlerts(Reply, Parsed.Alerts);
  return Parsed;
}

ERedwoodItemTradeOutcome URedwoodServerGameSubsystem::ClassifyItemsTradeReply(
  const FRedwoodItemTradeReply &Reply, int64 FromSeqBefore, int64 ToSeqBefore
) {
  // The flags come first: a reply that says it may have applied is never read as a refusal, and
  // never as a commit whose fences could be filler.
  if (!Reply.bReadable || Reply.bTimedOut || Reply.bCommitUnknown) {
    return ERedwoodItemTradeOutcome::Unknown;
  }
  if (Reply.bNotForwarded) {
    return ERedwoodItemTradeOutcome::NotForwarded;
  }
  if (Reply.Error.IsEmpty()) {
    // A success whose fences do not pass the seqs before the trade cannot be this trade's commit;
    // adopting it could leave a lane on a seq the trade used, where the next flush of item rows is
    // acked as a replay and never saved. The resend's replay ack carries the real fences.
    if (Reply.FromCommittedSeq > FromSeqBefore && Reply.ToCommittedSeq > ToSeqBefore) {
      return ERedwoodItemTradeOutcome::Committed;
    }
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT("Trade reply has no error but fences %lld/%lld do not pass the seqs before the trade (%lld/%lld)"),
      Reply.FromCommittedSeq,
      Reply.ToCommittedSeq,
      FromSeqBefore,
      ToSeqBefore
    );
    return ERedwoodItemTradeOutcome::Unknown;
  }
  return ERedwoodItemTradeOutcome::Refused;
}

void URedwoodServerGameSubsystem::HandleItemsTradeReply(
  const FString &TradeId, uint64 Generation, const TSharedPtr<FJsonObject> &Reply
) {
  // A reply to an attempt that timed out is late: a newer attempt is on its way, and only its reply
  // can say what the backend holds now.
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade || Trade->Generation != Generation) {
    UE_LOG(LogRedwood, Log, TEXT("Ignoring a late reply for trade %s"), *TradeId);
    return;
  }
  Trade->Generation = 0;
  if (UGameInstance *GameInstance = GetGameInstance()) {
    GameInstance->GetTimerManager().ClearTimer(Trade->Timer);
  }

  const FRedwoodItemTradeReply Parsed = ParseItemsTradeReply(Reply);
  if (!Parsed.Error.IsEmpty()) {
    UE_LOG(LogRedwood, Error, TEXT("Failed to trade items (trade %s): %s"), *TradeId, *Parsed.Error);
  }
  switch (ClassifyItemsTradeReply(Parsed, Trade->From.SeqBefore, Trade->To.SeqBefore)) {
    case ERedwoodItemTradeOutcome::Committed:
      CommitItemsTrade(TradeId, Parsed);
      return;
    case ERedwoodItemTradeOutcome::Unknown:
      Trade->bOutcomeUnknown = true;
      RetryItemsTrade(TradeId, Parsed.Error);
      return;
    case ERedwoodItemTradeOutcome::NotForwarded:
      // This attempt never reached the realm, so it says nothing about an earlier one.
      if (Trade->bOutcomeUnknown) {
        RetryItemsTrade(TradeId, Parsed.Error);
        return;
      }
      break;
    default:
      // After an attempt whose outcome is unknown, a refusal of the resend is proof that nothing
      // applied: the realm checks for a committed trade (its receipt, or its ledger rows) after
      // every refusal it makes inside its transaction, and acks a committed one as a replay. Its
      // refusals before the transaction are the same on every attempt, so the first attempt was
      // refused too, except the two that depend on this server's session: those say nothing about
      // the earlier attempt, so the trade is sent again.
      if (Trade->Attempts > 1 && Parsed.Error.Equals(TradeIdReusedError, ESearchCase::CaseSensitive)) {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Trade %s: the realm says a resend differs from its first send, but a resend is the same request. This is a bug."),
          *TradeId
        );
        GiveUpItemsTrade(TradeId, TEXT("a resend was refused as a reused trade id"));
        return;
      }
      if (Trade->bOutcomeUnknown && Parsed.bSessionRefused) {
        RetryItemsTrade(TradeId, Parsed.Error);
        return;
      }
      break;
  }

  // NOT applied: nothing is dropped or applied. Entries staged during the trade are re-chained from
  // the balance before it, which is still the backend's, and the game is told. Copies: the game
  // reacts to the rebase and may start another trade, which can move this one in the map.
  const FRedwoodItemTradeSide Sides[] = {Trade->From, Trade->To};
  for (const FRedwoodItemTradeSide &Side : Sides) {
    URedwoodCharacterComponent *Component = Side.CharacterComponent.Get();
    if (IsValid(Component) && Side.Entries.Num() > 0) {
      Component->RechainCurrencyAfterTrade(Side.BalanceBefore);
    }
  }
  FinishItemsTrade(TradeId, Parsed.Error, -1, -1);
}

void URedwoodServerGameSubsystem::HandleItemsTradeTimeout(const FString &TradeId, uint64 Generation) {
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade || Trade->Generation != Generation) {
    return;
  }
  // From here a reply to this attempt is late, and is ignored.
  Trade->Generation = 0;
  Trade->bOutcomeUnknown = true;
  UE_LOG(LogRedwood, Error, TEXT("Trade %s got no reply"), *TradeId);
  RetryItemsTrade(TradeId, TEXT("no reply"));
}

void URedwoodServerGameSubsystem::RetryItemsTrade(const FString &TradeId, const FString &Reason) {
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade) {
    return;
  }
  if (Trade->Attempts >= TradeMaxAttempts) {
    GiveUpItemsTrade(
      TradeId, FString::Printf(TEXT("%s; %d attempts made"), *Reason, Trade->Attempts)
    );
    return;
  }
  UGameInstance *GameInstance = GetGameInstance();
  if (!GameInstance) {
    GiveUpItemsTrade(TradeId, TEXT("no game instance to time the resend"));
    return;
  }
  GameInstance->GetTimerManager().SetTimer(
    Trade->Timer,
    FTimerDelegate::CreateWeakLambda(this, [this, TradeId]() { SendItemsTrade(TradeId); }),
    TradeResendSeconds,
    false // no loop
  );
}

void URedwoodServerGameSubsystem::ResumeItemsTradesAfterSidecarConnect() {
  // Keys first: a send can end its trade and change the map.
  TArray<FString> WaitingTradeIds;
  for (const TPair<FString, FRedwoodPendingItemTrade> &Pair : PendingItemTrades) {
    if (Pair.Value.bWaitingForSidecar) {
      WaitingTradeIds.Add(Pair.Key);
    }
  }
  for (const FString &TradeId : WaitingTradeIds) {
    SendItemsTrade(TradeId);
  }
}

void URedwoodServerGameSubsystem::CommitItemsTrade(
  const FString &TradeId, const FRedwoodItemTradeReply &Reply
) {
  const FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade) {
    return;
  }
  // Copies: the game reacts to each side's rebase and may start another trade, which can move this
  // one in the map. The trade stays in the map meanwhile, so both lanes stay held until both sides
  // have taken the commit.
  const FRedwoodItemTradeSide From = Trade->From;
  const FRedwoodItemTradeSide To = Trade->To;

  // Each side takes the commit the way a flush ack does: the seq forward, and the balance only when
  // the fence is exactly the trade's own commit. A replay ack carries the CURRENT fences, so a later
  // fence means something else committed after the trade, and the balance is then reloaded.
  auto CommitSide = [](const FRedwoodItemTradeSide &Side, int64 CommittedSeq) {
    FRedwoodItemFlushReply AsFlushReply;
    AsFlushReply.CommittedSeq = CommittedSeq;
    FRedwoodItemFlushSent AsFlushSent;
    AsFlushSent.BatchSeq = Side.SeqBefore + 1;
    AsFlushSent.bHasBalanceAfter = Side.Entries.Num() > 0;
    if (AsFlushSent.bHasBalanceAfter) {
      AsFlushSent.BalanceAfter = Side.Entries.Last().BalanceAfter;
    }
    if (URedwoodPlayerStateComponent *PlayerStateComponent = Side.PlayerStateComponent.Get()) {
      WriteCommittedFlushToSnapshot(PlayerStateComponent->RedwoodCharacter, AsFlushReply, AsFlushSent);
    }
    URedwoodCharacterComponent *Component = Side.CharacterComponent.Get();
    if (!IsValid(Component)) {
      return;
    }
    Component->AdoptCommittedItemSeq(CommittedSeq);
    if (!AsFlushSent.bHasBalanceAfter) {
      return;
    }
    if (CommittedSeq == AsFlushSent.BatchSeq) {
      Component->RechainCurrencyAfterTrade(AsFlushSent.BalanceAfter);
    } else {
      Component->RequireCurrencyReload();
    }
  };
  CommitSide(From, Reply.FromCommittedSeq);
  CommitSide(To, Reply.ToCommittedSeq);

  // Each alert goes to the character it names, so the game forwards it once.
  for (const TSharedPtr<FJsonObject> &Alert : Reply.Alerts) {
    const TSharedPtr<FJsonObject> *Detail = nullptr;
    FString OwnerId;
    if (Alert->TryGetObjectField(TEXT("detail"), Detail) && Detail) {
      (*Detail)->TryGetStringField(TEXT("ownerId"), OwnerId);
    }
    const FRedwoodItemTradeSide &Side = OwnerId == To.CharacterId ? To : From;
    if (URedwoodCharacterComponent *Component = Side.CharacterComponent.Get()) {
      Component->OnCurrencyAlerts.Broadcast({Alert});
    }
  }

  FinishItemsTrade(TradeId, FString(), Reply.FromCommittedSeq, Reply.ToCommittedSeq);
}

void URedwoodServerGameSubsystem::GiveUpItemsTrade(const FString &TradeId, const FString &Reason) {
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade) {
    return;
  }

  // Staff rebuild the trade from this line: it may have committed or not, and only the database
  // can tell. A re-grant must reuse the entry ids, so the backend refuses a second copy.
  TArray<FString> EntryLines;
  for (const FRedwoodItemTradeSide *Side : {&Trade->From, &Trade->To}) {
    for (const FRedwoodCurrencyEntry &Entry : Side->Entries) {
      EntryLines.Add(FString::Printf(
        TEXT("character %s entry %s delta %lld"), *Side->CharacterId, *Entry.Id, Entry.Delta
      ));
    }
  }
  UE_LOG(
    LogRedwood,
    Error,
    TEXT("Trade %s (%s -> %s) gave up (%s). Its outcome is UNKNOWN; both characters freeze. Currency: [%s]"),
    *TradeId,
    *Trade->From.CharacterId,
    *Trade->To.CharacterId,
    *Reason,
    *FString::Join(EntryLines, TEXT("; "))
  );

  // The same freeze as a final refusal: the balance and the seq of both characters are unknown, so
  // nothing staged can be sent, and only a fresh load from the database repairs them. Nothing is in
  // flight: the trade held both lanes. Both freeze before the game hears of either, so its reaction
  // to one finds the other frozen too. Copies, because that reaction may move this trade in the map.
  const FString Error = FString::Printf(TEXT("trade %s outcome unknown: %s"), *TradeId, *Reason);
  const FRedwoodItemTradeSide Sides[] = {Trade->From, Trade->To};
  for (const FRedwoodItemTradeSide &Side : Sides) {
    FreezeCharacter(Side.CharacterId, Side.PlayerStateComponent.Get());
    if (URedwoodCharacterComponent *Component = Side.CharacterComponent.Get()) {
      Component->FreezeAfterFinalRefusal(Error);
    }
  }
  for (const FRedwoodItemTradeSide &Side : Sides) {
    if (URedwoodCharacterComponent *Component = Side.CharacterComponent.Get()) {
      Component->OnItemFlushRefusedFinal.Broadcast(Side.CharacterId, Error);
    }
  }
  FinishItemsTrade(TradeId, Error, -1, -1);
}

void URedwoodServerGameSubsystem::FinishItemsTrade(
  const FString &TradeId, const FString &Error, int64 FromCommittedSeq, int64 ToCommittedSeq
) {
  FRedwoodPendingItemTrade *Trade = PendingItemTrades.Find(TradeId);
  if (!Trade) {
    return;
  }
  if (UGameInstance *GameInstance = GetGameInstance()) {
    GameInstance->GetTimerManager().ClearTimer(Trade->Timer);
  }
  // Out of the map first: the caller and the resumed flushes may start a new trade or flush, and
  // must not be held by this finished one.
  FRedwoodPendingItemTrade Finished = MoveTemp(*Trade);
  PendingItemTrades.Remove(TradeId);
  if (Finished.OnComplete) {
    Finished.OnComplete(Error, FromCommittedSeq, ToCommittedSeq);
  }
  ResumeHeldItemFlushes(Finished.HeldFlushes);
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

// FORK(hollowed-oath) BEGIN: account document calls. The checks, the reply
// parsing and the offline store are in RedwoodAccountDocuments.cpp.
void URedwoodServerGameSubsystem::GetAccountDocument(
  const FString &PlayerId,
  const FString &Key,
  FRedwoodGetAccountDocumentOutputDelegate OnComplete
) {
  if (!RedwoodAccountDocuments::IsValidId(PlayerId) ||
      !RedwoodAccountDocuments::IsValidKey(Key)) {
    FRedwoodGetAccountDocumentOutput Output;
    Output.Error = TEXT("Invalid player id or key");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  // ShouldUseBackend is false for a null world. Without this check, a call
  // with no world would use the offline store while the backend never sees it.
  if (!GetWorld()) {
    FRedwoodGetAccountDocumentOutput Output;
    Output.Error = TEXT("No world");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
    OnComplete.ExecuteIfBound(RedwoodAccountDocuments::ReadOffline(PlayerId, Key));
    return;
  }

  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    FRedwoodGetAccountDocumentOutput Output;
    Output.Error = TEXT("Sidecar is not connected");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("key"), Key);

  Sidecar->Emit(
    TEXT("realm:account-documents:get"),
    Payload,
    [OnComplete](auto Response) {
      OnComplete.ExecuteIfBound(RedwoodAccountDocuments::ParseGetReply(Response));
    }
  );
}

void URedwoodServerGameSubsystem::PutAccountDocument(
  const FString &PlayerId,
  const FString &Key,
  TSharedPtr<FJsonObject> Data,
  int32 ExpectedRevision,
  FRedwoodPutAccountDocumentOutputDelegate OnComplete
) {
  FRedwoodPutAccountDocumentOutput Output;

  if (!RedwoodAccountDocuments::IsValidId(PlayerId) ||
      !RedwoodAccountDocuments::IsValidKey(Key)) {
    Output.Error = TEXT("Invalid player id or key");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  if (!Data.IsValid() || ExpectedRevision < 0) {
    Output.Error = TEXT("Account document data and a revision of 0 or more are required");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  // Checked before the size: the size check serializes Data, and the
  // serializer recurses. Checked here, not only by the backend, so the
  // offline path refuses the same documents.
  if (RedwoodAccountDocuments::IsTooDeep(Data.ToSharedRef())) {
    Output.Error = TEXT("Account document is nested too deep");
    Output.bRejected = true;
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  // Checked before the send: a message above socket.io's buffer limit is
  // dropped by the socket, and the caller would only see a timeout.
  if (RedwoodAccountDocuments::IsTooLarge(Data.ToSharedRef())) {
    Output.Error = TEXT("Account document is too large");
    Output.bRejected = true;
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  // See GetAccountDocument: no world must not fall back to the offline store.
  if (!GetWorld()) {
    Output.Error = TEXT("No world");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  if (!URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
    OnComplete.ExecuteIfBound(RedwoodAccountDocuments::WriteOffline(
      PlayerId, Key, Data.ToSharedRef(), ExpectedRevision
    ));
    return;
  }

  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    Output.Error = TEXT("Sidecar is not connected");
    OnComplete.ExecuteIfBound(Output);
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("playerId"), PlayerId);
  Payload->SetStringField(TEXT("key"), Key);
  Payload->SetNumberField(TEXT("expectedRevision"), ExpectedRevision);
  Payload->SetObjectField(TEXT("data"), Data);

  Sidecar->Emit(
    TEXT("realm:account-documents:put"),
    Payload,
    [OnComplete](auto Response) {
      OnComplete.ExecuteIfBound(RedwoodAccountDocuments::ParsePutReply(Response));
    }
  );
}
// FORK(hollowed-oath) END

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

// FORK(hollowed-oath) BEGIN: the in-game /gm command. The three functions below
// let the game server ask the backend to change the account role of a player.
// All fork-added, and built on the same sidecar request pattern as GetParty
// above. The request is shaped in one function and the answer read in another,
// so the automation test can reach both rules without a backend.
//
// This code decides NOTHING. The director alone applies the rank rule and
// writes the role, so the plugin only shapes the request and hands the answer
// back word for word. Keeping the plugin free of role knowledge is what lets
// the game add a tier, or change what a player reads, with no fork change here.
TSharedPtr<FJsonObject> URedwoodServerGameSubsystem::MakeSetPlayerRolePayload(
  const FString &TargetPlayerId,
  const FString &RoleKey,
  const FString &ActorPlayerId
) {
  TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
  Payload->SetStringField(TEXT("targetPlayerId"), TargetPlayerId);
  Payload->SetStringField(TEXT("actorPlayerId"), ActorPlayerId);

  // An empty key clears the role, and the wire must then carry an explicit
  // null. The backend declares roleKey as a field that must be present, so a
  // key that is simply left out is refused as a malformed request.
  if (RoleKey.IsEmpty()) {
    Payload->SetField(TEXT("roleKey"), MakeShareable(new FJsonValueNull()));
  } else {
    Payload->SetStringField(TEXT("roleKey"), RoleKey);
  }

  return Payload;
}

void URedwoodServerGameSubsystem::AnswerSetPlayerRole(
  const TArray<TSharedPtr<FJsonValue>> &Response,
  FRedwoodSetPlayerRoleOutputDelegate OnOutput
) {
  const TSharedPtr<FJsonObject> *MessageObject =
    URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
  if (MessageObject == nullptr) {
    OnOutput.ExecuteIfBound(
      false, 0, TEXT("The backend did not answer the role change")
    );
    return;
  }

  // The backend puts an error on EVERY answer, empty when the change is made.
  // An object without the field is therefore not an answer of this backend,
  // and must not be read as an empty error -- that is the same false success
  // an empty object gives, which the test above exists to stop.
  FString Error;
  if (!(*MessageObject)->TryGetStringField(TEXT("error"), Error)) {
    OnOutput.ExecuteIfBound(
      false, 0, TEXT("The backend did not answer the role change")
    );
    return;
  }

  // The tier the backend committed, which is not always the tier that was
  // asked for. It is absent on every refusal, and then stays 0.
  //
  // The range test is the one ParsePlayerData explains: a JSON number arrives
  // as a double, and narrowing one that is out of range is undefined
  // behaviour. A number this fork's own backend cannot send, so the answer is
  // still reported: the error string decides whether the change took effect,
  // and a change that DID take effect must never be reported as a failure --
  // an operator would then ask again for a change that is already made.
  int32 NewRole = 0;
  double RoleValue = 0.0;
  if ((*MessageObject)->TryGetNumberField(TEXT("role"), RoleValue) &&
      RoleValue >= static_cast<double>(MIN_int32) &&
      RoleValue <= static_cast<double>(MAX_int32)) {
    NewRole = static_cast<int32>(RoleValue);
  }

  OnOutput.ExecuteIfBound(Error.IsEmpty(), NewRole, Error);
}

void URedwoodServerGameSubsystem::RequestPlayerRoleChange(
  const FString &TargetPlayerId,
  const FString &RoleKey,
  const FString &ActorPlayerId,
  FRedwoodSetPlayerRoleOutputDelegate OnOutput
) {
  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    OnOutput.ExecuteIfBound(false, 0, TEXT("Sidecar is not connected"));
    return;
  }

  Sidecar->Emit(
    SetPlayerRoleEventName,
    MakeSetPlayerRolePayload(TargetPlayerId, RoleKey, ActorPlayerId),
    [OnOutput](auto Response) { AnswerSetPlayerRole(Response, OnOutput); }
  );
}

FRedwoodListOnlineCharactersOutput
URedwoodServerGameSubsystem::ParseListOnlineCharacters(
  const TArray<TSharedPtr<FJsonValue>> &Response
) {
  FRedwoodListOnlineCharactersOutput Output;
  Output.Error = TEXT("The backend did not answer the online character list");

  const TSharedPtr<FJsonObject> *MessageObject =
    URedwoodCommonGameSubsystem::TryGetRedwoodAnswerObject(Response);
  FString Error;
  if (MessageObject == nullptr ||
      !(*MessageObject)->TryGetStringField(TEXT("error"), Error)) {
    return Output;
  }

  // The backend puts the array on EVERY answer, empty on a refusal. An answer
  // without it is not one of its answers, and must keep the default error:
  // a success with no array would read as nobody online.
  const TArray<TSharedPtr<FJsonValue>> *Characters = nullptr;
  if (!(*MessageObject)->TryGetArrayField(TEXT("characters"), Characters)) {
    return Output;
  }

  Output.Error = Error;
  Output.Characters.Reserve(Characters->Num());
  for (const TSharedPtr<FJsonValue> &Value : *Characters) {
    const TSharedPtr<FJsonObject> *Entry = nullptr;
    FRedwoodOnlineCharacter Character;
    if (Value.IsValid() && Value->TryGetObject(Entry) &&
        (*Entry)->TryGetStringField(TEXT("name"), Character.Name)) {
      (*Entry)->TryGetStringField(TEXT("zoneName"), Character.ZoneName);
      Output.Characters.Add(MoveTemp(Character));
    }
  }

  return Output;
}

void URedwoodServerGameSubsystem::RequestOnlineCharacters(
  FRedwoodListOnlineCharactersOutputDelegate OnOutput
) {
  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    FRedwoodListOnlineCharactersOutput Output;
    Output.Error = TEXT("Sidecar is not connected");
    OnOutput.ExecuteIfBound(Output);
    return;
  }

  Sidecar->Emit(
    ListOnlineCharactersEventName,
    MakeShareable(new FJsonObject),
    [OnOutput](auto Response) {
      OnOutput.ExecuteIfBound(ParseListOnlineCharacters(Response));
    }
  );
}
// FORK(hollowed-oath) END

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
