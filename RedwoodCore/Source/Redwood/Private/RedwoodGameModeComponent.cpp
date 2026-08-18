// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodGameModeComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodGameplayTags.h"
#include "RedwoodModule.h"
#include "RedwoodPlayerStateComponent.h"
#include "RedwoodServerGameSubsystem.h"
#include "RedwoodZoneSpawn.h"

#if WITH_EDITOR
  #include "RedwoodEditorSettings.h"
#endif

#include "Dom/JsonObject.h"
// FORK(hollowed-oath): Character + CapsuleComponent for RetryFailedPawnSpawn.
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Guid.h"
#include "Net/OnlineEngineInterface.h"

#include "SocketIOClient.h"

URedwoodGameModeComponent::URedwoodGameModeComponent(
  const FObjectInitializer &ObjectInitializer
) :
  Super(ObjectInitializer) {
  PrimaryComponentTick.bStartWithTickEnabled = true;
  PrimaryComponentTick.bCanEverTick = true;
}

void URedwoodGameModeComponent::BeginPlay() {
  Super::BeginPlay();

  UE_LOG(
    LogRedwood,
    Log,
    TEXT(
      "RedwoodGameModeComponent BeginPlay, will attempt to run PostBeginPlay every %f seconds until initialized"
    ),
    PostBeginPlayDelay
  );

  FTimerManager &TimerManager = GetWorld()->GetTimerManager();
  TimerManager.SetTimer(
    PostBeginPlayTimerHandle,
    this,
    &URedwoodGameModeComponent::PostBeginPlay,
    PostBeginPlayDelay,
    true
  );
}

void URedwoodGameModeComponent::PostBeginPlay() {
  ServerSubsystem =
    GetWorld()->GetGameInstance()->GetSubsystem<URedwoodServerGameSubsystem>();

  if (ServerSubsystem) {
    UE_LOG(
      LogRedwood,
      Log,
      TEXT(
        "URedwoodGameModeComponent::PostBeginPlay: Valid RedwoodServerGameSubsystem found, finishing initialization"
      )
    );

    FTimerManager &TimerManager = GetWorld()->GetTimerManager();
    TimerManager.ClearTimer(PostBeginPlayTimerHandle);

    ServerSubsystem->InitialDataLoad(FRedwoodDelegate::CreateLambda([this]() {
      UE_LOG(
        LogRedwood,
        Log,
        TEXT(
          "URedwoodGameModeComponent::PostBeginPlay: Initial data load complete"
        )
      );

      bPostBeganPlay = true;

      // create a looping timer to flush persistent data
      if (DatabasePersistenceInterval > 0) {
        FTimerManager &TimerManager = GetWorld()->GetTimerManager();
        TimerManager.SetTimer(
          FlushPersistentDataTimerHandle,
          this,
          &URedwoodGameModeComponent::FlushPersistence,
          DatabasePersistenceInterval,
          true
        );
      }
    }));
  } else {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "URedwoodGameModeComponent::PostBeginPlay: Invalid RedwoodServerGameSubsystem (likely during world initialization); will retry shortly"
      )
    );
  }
}

void URedwoodGameModeComponent::TickComponent(
  float DeltaTime,
  enum ELevelTick TickType,
  FActorComponentTickFunction *ThisTickFunction
) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  if (bPostBeganPlay && !GetWorld()->bIsTearingDown) {
    if (URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
      URedwoodServerGameSubsystem *RedwoodServerGameSubsystem =
        GetWorld()
          ->GetGameInstance()
          ->GetSubsystem<URedwoodServerGameSubsystem>();

      RedwoodServerGameSubsystem->FlushSync();
    }
  }
}

void URedwoodGameModeComponent::FlushPersistence() {
  ServerSubsystem->FlushPersistence();
}

void URedwoodGameModeComponent::InitGame(
  const FString &MapName, const FString &Options, FString &ErrorMessage
) {
  if (URedwoodCommonGameSubsystem::ShouldUseBackend(GetWorld())) {
    Sidecar = ISocketIOClientModule::Get().NewValidNativePointer();

    if (!ServerSubsystem) {
      ServerSubsystem = GetWorld()
                          ->GetGameInstance()
                          ->GetSubsystem<URedwoodServerGameSubsystem>();
    }

    if (ServerSubsystem) {
      Sidecar->Connect(ServerSubsystem->SidecarUri);
    } else {
      UE_LOG(
        LogRedwood,
        Error,
        TEXT("Invalid RedwoodServerGameSubsystem; cannot connect to sidecar")
      );
    }
  }

  FGameModeEvents::GameModeLogoutEvent.AddUObject(
    this, &URedwoodGameModeComponent::OnGameModeLogout
  );
}

void URedwoodGameModeComponent::OnGameModeLogout(
  AGameModeBase *GameMode, AController *Controller
) {
  APlayerController *PlayerController = Cast<APlayerController>(Controller);
  if (PlayerController == nullptr) {
    return;
  }

  URedwoodPlayerStateComponent *PlayerStateComponent =
    IsValid(PlayerController->PlayerState)
    ? PlayerController->PlayerState
        ->FindComponentByClass<URedwoodPlayerStateComponent>()
    : nullptr;
  if (IsValid(PlayerStateComponent)) {
    if (ServerSubsystem) {
      TArray<APlayerState *> PlayerFlushArray;
      PlayerFlushArray.Add(PlayerController->PlayerState);
      ServerSubsystem->FlushPlayerCharacterData(PlayerFlushArray, true);
    }

    if (URedwoodCommonGameSubsystem::ShouldUseBackend(GameMode->GetWorld())) {
      if (Sidecar.IsValid() && Sidecar->bIsConnected) {
        // FORK(hollowed-oath): fork-added block inside the otherwise-stock
        // logout emit (upstream emits the same player-left with no flag).
        // On upstream merge, keep this evaluation + the conditional
        // retainBinding field on the payload below.
        //
        // A game whose character stays in-world past the connection (e.g.
        // linkdead body retention) asks the backend to keep the
        // character->instance write binding: releasing it now would revoke
        // the character writes the game still owes (its final flush).
        // Presence processing (party/chat/director) is unaffected — the
        // player-left is always emitted here. The game releases the binding
        // later via URedwoodServerGameSubsystem::EmitPlayerLeft. Evaluated
        // only when a player-left will actually be emitted.
        const bool bRetainBinding = ShouldRetainCharacterBinding.IsBound() &&
          ShouldRetainCharacterBinding.Execute(PlayerController);

        TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
        JsonObject->SetStringField(
          TEXT("playerId"), PlayerStateComponent->RedwoodCharacter.PlayerId
        );
        JsonObject->SetStringField(
          TEXT("characterId"), PlayerStateComponent->RedwoodCharacter.Id
        );
        if (bRetainBinding) {
          JsonObject->SetBoolField(TEXT("retainBinding"), true);
        }
        Sidecar->Emit(
          TEXT("realm:servers:player-left:game-server-to-sidecar"), JsonObject
        );
      }
    }

    if (UGameplayMessageSubsystem::HasInstance(this)) {
      // When we stop PIE, it's possible for the subsystem to be destroyed
      // before we get this event, so we need to check if it's valid
      UGameplayMessageSubsystem &MessageSubsystem =
        UGameplayMessageSubsystem::Get(this);
      MessageSubsystem.BroadcastMessage(
        TAG_Redwood_Player_Left, FRedwoodPlayerLeft{PlayerController}
      );
    }
  }
}

APlayerController *URedwoodGameModeComponent::Login(
  UPlayer *NewPlayer,
  ENetRole InRemoteRole,
  const FString &Portal,
  const FString &Options,
  const FUniqueNetIdRepl &UniqueId,
  FString &ErrorMessage,
  std::function<APlayerController
                  *(UPlayer *,
                    ENetRole,
                    const FString &,
                    const FString &,
                    const FUniqueNetIdRepl &,
                    FString &)> SuperDelegate
) {
  APlayerController *PlayerController = SuperDelegate(
    NewPlayer, InRemoteRole, Portal, Options, UniqueId, ErrorMessage
  );

  if (!ErrorMessage.IsEmpty() || PlayerController == nullptr) {
    return PlayerController;
  }

  UWorld *World = GetWorld();
  AGameModeBase *GameMode = World->GetAuthGameMode();

  if (URedwoodCommonGameSubsystem::ShouldUseBackend(World)) {
    FString PlayerId;
    FString CharacterId;
    FString Token;

#if WITH_EDITOR
    if (World->WorldType == EWorldType::PIE) {
      // Redwood has an option that is only available when the backend
      // the "dev-debug" game server provider is being used.
      int32 PlayerIndex = GameMode->GameState->PlayerArray.Num() - 1;
      PlayerId = FString::Printf(TEXT("development_%d"), PlayerIndex);
      CharacterId = FString::Printf(TEXT("development_%d"), PlayerIndex);
      Token = TEXT("development");
    }
#endif

    if (UGameplayStatics::HasOption(Options, TEXT("RedwoodAuth"))) {
      PlayerId = UGameplayStatics::ParseOption(Options, TEXT("PlayerId"));
      CharacterId = UGameplayStatics::ParseOption(Options, TEXT("CharacterId"));
      Token = UGameplayStatics::ParseOption(Options, TEXT("Token"));
    }

    if (!PlayerId.IsEmpty() && !CharacterId.IsEmpty() && !Token.IsEmpty()) {
      // The join token is carried in the connection URL options (see
      // URedwoodClientInterface::GetConnectionURL). Verify it against
      // the sidecar; RunSidecarPlayerAuth kicks the player on failure.
      RunSidecarPlayerAuth(PlayerController, PlayerId, CharacterId, Token);
    } else {
      ErrorMessage =
        TEXT("Invalid authentication request: missing RedwoodAuth option");
    }
  } else {
    // we're likely PIE so just load the character from disk based on num players
    uint8 PlayerIndex = 0;
    if (IsValid(GameMode->GameState)) {
      PlayerIndex = GameMode->GameState->PlayerArray.Num() - 1;
    } else {
      FString ErrorGameState = TEXT(
        "GameState is not valid yet, defaulting to player index 0 for character loading"
      );

      UE_LOG(LogRedwood, Error, TEXT("%s"), *ErrorGameState);

      FRedwoodModule::ShowNotification(ErrorGameState);
    }

    TArray<FRedwoodCharacterBackend> Characters =
      URedwoodCommonGameSubsystem::LoadAllCharactersFromDisk();

    if (PlayerIndex < Characters.Num()) {
      URedwoodPlayerStateComponent *PlayerStateComponent =
        PlayerController->PlayerState
          ->FindComponentByClass<URedwoodPlayerStateComponent>();
      if (IsValid(PlayerStateComponent)) {
        FRedwoodCharacterBackend &Character = Characters[PlayerIndex];
        PlayerStateComponent->SetRedwoodCharacter(Character);

        PlayerStateComponent->SetServerReady();

        // NOTE: we do not call HandleStartingNewPlayer here because we
        // don't need to. This all runs synchronously and PostLogin will
        // call HandleStartingNewPlayer for us. We only call HandleStartingNewPlayer
        // above because of the asynchronous nature of the backend call
      } else {
        UE_LOG(
          LogRedwood,
          Log,
          TEXT("Can't load character data as we're not using RedwoodPlayerStateComponent"
          )
        );

        FRedwoodModule::ShowNotification(TEXT(
          "Can't load character data as we're not using RedwoodPlayerStateComponent"
        ));
      }
    } else {
      ErrorMessage = FString::Printf(
        TEXT("No character found for this player index %d"), PlayerIndex
      );
      FString SubText =
        TEXT("Did you create one in Standalone with bUseBackendInPIE = false?");

      FRedwoodModule::ShowNotification(
        ErrorMessage, 10.0f, true, true, SubText
      );
    }
  }

  return PlayerController;
}

void URedwoodGameModeComponent::PostLogin(APlayerController *NewPlayer) {
  URedwoodPlayerStateComponent *PlayerStateComponent =
    NewPlayer->PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>(
    );
  if (IsValid(PlayerStateComponent)) {
    PlayerStateComponent->bRanPostLogin = true;
  }
}

TArray<FString> URedwoodGameModeComponent::GetExpectedCharacterIds() const {
  TArray<FString> ExpectedCharacterIds;

  TArray<FString> Options = GetWorld()->URL.Op;
  for (const FString &Option : Options) {
    if (Option.StartsWith(TEXT("redwoodExpectedCharacterIds="))) {
      FString AllIds = Option.RightChop(28);

      for (int32 IdStart = 0, IdEnd = 0; IdEnd != INDEX_NONE;
           IdStart = IdEnd + 1) {
        IdEnd = AllIds.Find(
          TEXT(","), ESearchCase::IgnoreCase, ESearchDir::FromStart, IdStart
        );
        if (IdEnd == INDEX_NONE) {
          ExpectedCharacterIds.Add(AllIds.RightChop(IdStart));
          break;
        }
        ExpectedCharacterIds.Add(AllIds.Mid(IdStart, IdEnd - IdStart));
      }

      break;
    }
  }

  return ExpectedCharacterIds;
}

bool URedwoodGameModeComponent::PlayerCanRestart_Implementation(
  APlayerController *Player,
  std::function<bool(APlayerController *)> SuperDelegate
) {
  URedwoodPlayerStateComponent *PlayerStateComponent =
    Player->PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>();
  if (IsValid(PlayerStateComponent)) {
    if (!PlayerStateComponent->bServerReady) {
      return false;
    }
  }

  return SuperDelegate(Player);
}

void URedwoodGameModeComponent::FinishRestartPlayer(
  AController *NewPlayer,
  const FRotator &StartRotation,
  std::function<void(AController *)> FailedToRestartPlayerDelegate
) {
  NewPlayer->Possess(NewPlayer->GetPawn());

  // If the Pawn is destroyed as part of possession we have to abort
  if (!IsValid(NewPlayer->GetPawn())) {
    FailedToRestartPlayerDelegate(NewPlayer);
  } else {
    URedwoodPlayerStateComponent *PlayerStateComponent =
      NewPlayer->PlayerState
        ->FindComponentByClass<URedwoodPlayerStateComponent>();

    FRotator NewControlRotation = NewPlayer->GetPawn()->GetActorRotation();

    if (IsValid(PlayerStateComponent)) {
      FTransform OutTransform;
      FRotator OutControlRotation;
      if (PlayerStateComponent->GetSpawnData(
            OutTransform, OutControlRotation
          )) {
        NewControlRotation = OutControlRotation;
      }
    }

    NewPlayer->ClientSetRotation(NewControlRotation, true);
    NewPlayer->SetControlRotation(NewControlRotation);

    AGameModeBase *GameMode = GetWorld()->GetAuthGameMode();
    GameMode->SetPlayerDefaults(NewPlayer->GetPawn());
    GameMode->K2_OnRestartPlayer(NewPlayer);
  }
}

FTransform URedwoodGameModeComponent::PickPawnSpawnTransform(
  AController *NewPlayer, const FTransform &SpawnTransform
) {
  URedwoodServerGameSubsystem *RedwoodServerGameSubsystem =
    NewPlayer->GetWorld()
      ->GetGameInstance()
      ->GetSubsystem<URedwoodServerGameSubsystem>();

  // get all actors of the ARedwoodZoneSpawn class
  TArray<AActor *> ZoneSpawns;
  UGameplayStatics::GetAllActorsOfClass(
    NewPlayer->GetWorld(), ARedwoodZoneSpawn::StaticClass(), ZoneSpawns
  );

  FString ZoneName = RedwoodServerGameSubsystem->ZoneName;

#if WITH_EDITOR
  const URedwoodEditorSettings *EditorSettings =
    GetDefault<URedwoodEditorSettings>();

  if (
    NewPlayer->GetWorld()->WorldType == EWorldType::PIE &&
    EditorSettings->bUseBackendInPIE == false &&
    ZoneName.IsEmpty() &&
    !EditorSettings->FallbackZoneName.IsEmpty()
  ) {
    ZoneName = EditorSettings->FallbackZoneName;
  }
#endif

  TArray<ARedwoodZoneSpawn *> RedwoodZoneSpawns;
  for (AActor *ZoneSpawn : ZoneSpawns) {
    ARedwoodZoneSpawn *RedwoodZoneSpawn = Cast<ARedwoodZoneSpawn>(ZoneSpawn);
    if (IsValid(RedwoodZoneSpawn)) {
      if (RedwoodZoneSpawn->ZoneName == ZoneName) {
        RedwoodZoneSpawns.Add(RedwoodZoneSpawn);
      }
    }
  }

  URedwoodPlayerStateComponent *PlayerStateComponent =
    NewPlayer->PlayerState->FindComponentByClass<URedwoodPlayerStateComponent>(
    );

  if (IsValid(PlayerStateComponent)) {
    FTransform OutTransform;
    FRotator OutControlRotation;
    if (PlayerStateComponent->GetSpawnData(OutTransform, OutControlRotation)) {
      return OutTransform;
    }

    UE_LOG(
      LogRedwood,
      Log,
      TEXT("No valid last transform found, using default zone spawn")
    );

    for (ARedwoodZoneSpawn *ZoneSpawn : RedwoodZoneSpawns) {
      if (ZoneSpawn->SpawnName == TEXT("default")) {
        return ZoneSpawn->GetSpawnTransform();
      }
    }

    if (RedwoodZoneSpawns.Num() > 0) {
      FRedwoodModule::ShowNotification(TEXT(
        "Could not find ARedwoodZoneSpawn with SpawnName 'default', using first available spawn"
      ));

      return RedwoodZoneSpawns[0]->GetSpawnTransform();
    } else {
      bool bShowNotification = true;

#if WITH_EDITOR
      bShowNotification = EditorSettings->bUseBackendInPIE ||
        !EditorSettings->FallbackZoneName.IsEmpty();
#endif

      FString NotificationText = FString::Printf(
        TEXT(
          "Could not find a valid spawn location for the player; using default transform (Loc %f, %f, %f)."
        ),
        SpawnTransform.GetLocation().X,
        SpawnTransform.GetLocation().Y,
        SpawnTransform.GetLocation().Z
      );

      if (bShowNotification) {
        FRedwoodModule::ShowNotification(NotificationText);
      }
    }
  } else {
    FString NotificationText = FString::Printf(
      TEXT(
        "Trying to spawn player without valid URedwoodPlayerStateComponent; using default transform (Loc %f, %f, %f)."
      ),
      SpawnTransform.GetLocation().X,
      SpawnTransform.GetLocation().Y,
      SpawnTransform.GetLocation().Z
    );
  }

  UE_LOG(
    LogRedwood,
    Error,
    TEXT(
      "Could not find a lastLocation for the character and there's no valid ARedwoodZoneSpawn found for this zone (%s). Using default transform."
    ),
    *RedwoodServerGameSubsystem->ZoneName
  );

  return SpawnTransform;
}

// FORK(hollowed-oath): see the rationale on the header declaration.
// FindPlayerStart alone is NOT safe as a fallback: with no APlayerStart in
// the map it returns the AWorldSettings actor at the world origin, and a
// force-spawn there writes (0,0,0) into lastLocation at the next flush,
// corrupting every later login for the character.
bool URedwoodGameModeComponent::ResolveFallbackArrivalTransform(
  AGameModeBase *GameMode, AController *NewPlayer, FTransform &OutTransform
) {
  UWorld *World = GetWorld();
  if (!IsValid(World) || !GameMode) {
    return false;
  }

  if (const APlayerStart *PlayerStart =
        Cast<APlayerStart>(GameMode->FindPlayerStart(NewPlayer))) {
    OutTransform = FTransform(
      PlayerStart->GetActorRotation(), PlayerStart->GetActorLocation()
    );
    return true;
  }

  // Redwood maps place zone spawns, not PlayerStarts. Prefer this zone's
  // "default" spawn; take any zone spawn over none.
  URedwoodServerGameSubsystem *Subsystem =
    World->GetGameInstance()
      ? World->GetGameInstance()->GetSubsystem<URedwoodServerGameSubsystem>()
      : nullptr;
  const FString CurrentZoneName = Subsystem ? Subsystem->ZoneName : FString();
  TArray<AActor *> ZoneSpawnActors;
  UGameplayStatics::GetAllActorsOfClass(
    World, ARedwoodZoneSpawn::StaticClass(), ZoneSpawnActors
  );
  ARedwoodZoneSpawn *BestZoneSpawn = nullptr;
  for (AActor *Actor : ZoneSpawnActors) {
    ARedwoodZoneSpawn *ZoneSpawn = Cast<ARedwoodZoneSpawn>(Actor);
    if (!IsValid(ZoneSpawn)) {
      continue;
    }
    if (!CurrentZoneName.IsEmpty() && ZoneSpawn->ZoneName != CurrentZoneName) {
      continue;
    }
    BestZoneSpawn = ZoneSpawn;
    if (ZoneSpawn->SpawnName == TEXT("default")) {
      break;
    }
  }
  if (BestZoneSpawn) {
    OutTransform = BestZoneSpawn->GetSpawnTransform();
    return true;
  }
  return false;
}

// FORK(hollowed-oath): recovery for a failed default-pawn spawn. See the
// rationale on the header declaration.
APawn *URedwoodGameModeComponent::RetryFailedPawnSpawn(
  AGameModeBase *GameMode,
  AController *NewPlayer,
  const FTransform &FailedTransform
) {
  UWorld *World = GetWorld();
  UClass *PawnClass = GameMode
    ? GameMode->GetDefaultPawnClassForController(NewPlayer)
    : nullptr;
  if (!IsValid(World) || !PawnClass) {
    return nullptr;
  }

  FActorSpawnParameters SpawnInfo;
  SpawnInfo.Instigator = GameMode->GetInstigator();
  SpawnInfo.ObjectFlags |= RF_Transient;

  // Attempt 1: the same spot lifted by the capsule half-height — the
  // observed failure is a ground-trace hit that left the capsule in the
  // floor, so headroom is the most likely cure. The lift can push the spot
  // into a ceiling in a low room; attempt 2 covers that. The collision
  // policy is DELIBERATELY adjust-or-fail (stricter than a class default of
  // AlwaysSpawn): an embedded pawn is the failure this function exists to
  // avoid, so a spot the engine cannot adjust must fall through to the
  // designed arrival point below, never force-spawn here.
  const ACharacter *PawnDefault = Cast<ACharacter>(PawnClass->GetDefaultObject());
  const float Lift = (PawnDefault && PawnDefault->GetCapsuleComponent())
    ? PawnDefault->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
    : ARedwoodZoneSpawn::LegacySpawnGroundClearance;
  FTransform LiftedTransform = FailedTransform;
  LiftedTransform.SetLocation(
    FailedTransform.GetLocation() + FVector(0.0f, 0.0f, Lift)
  );
  SpawnInfo.SpawnCollisionHandlingOverride =
    ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
  if (APawn *Pawn =
        World->SpawnActor<APawn>(PawnClass, LiftedTransform, SpawnInfo)) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "Default pawn spawn failed at (%s); recovered %.0f units higher."
      ),
      *FailedTransform.GetLocation().ToCompactString(),
      Lift
    );
    return Pawn;
  }

  // Attempt 2: a designed arrival point; null when the map has none — a
  // pawnless spectator beats a corrupted character record.
  FTransform FallbackTransform;
  if (!ResolveFallbackArrivalTransform(GameMode, NewPlayer, FallbackTransform)) {
    UE_LOG(
      LogRedwood,
      Warning,
      TEXT(
        "Default pawn spawn failed at (%s) and the map has no PlayerStart or zone spawn to fall back to."
      ),
      *FailedTransform.GetLocation().ToCompactString()
    );
    return nullptr;
  }

  SpawnInfo.SpawnCollisionHandlingOverride =
    ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
  APawn *Pawn =
    World->SpawnActor<APawn>(PawnClass, FallbackTransform, SpawnInfo);
  UE_LOG(
    LogRedwood,
    Warning,
    TEXT("Default pawn spawn failed at (%s); arrival-point fallback at (%s) %s."),
    *FailedTransform.GetLocation().ToCompactString(),
    *FallbackTransform.GetLocation().ToCompactString(),
    IsValid(Pawn) ? TEXT("succeeded") : TEXT("also failed")
  );
  return Pawn;
}

// ---------------------------------------------------------------------------
// Player auth verification against the sidecar
// ---------------------------------------------------------------------------

void URedwoodGameModeComponent::RunSidecarPlayerAuth(
  APlayerController *PlayerController,
  const FString &PlayerId,
  const FString &CharacterId,
  const FString &Token
) {
  if (!IsValid(PlayerController)) {
    return;
  }

  UWorld *World = GetWorld();
  AGameModeBase *GameMode = World ? World->GetAuthGameMode() : nullptr;
  if (GameMode == nullptr) {
    return;
  }

  if (!Sidecar.IsValid() || !Sidecar->bIsConnected) {
    UE_LOG(
      LogRedwood,
      Error,
      TEXT("Sidecar is not connected; kicking %s"),
      *PlayerId
    );
    GameMode->GameSession->KickPlayer(
      PlayerController, FText::FromString(TEXT("Sidecar is not connected"))
    );
    return;
  }

  TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
  JsonObject->SetStringField(TEXT("playerId"), PlayerId);
  JsonObject->SetStringField(TEXT("characterId"), CharacterId);
  JsonObject->SetStringField(TEXT("token"), Token);

  // The sidecar round-trip is async; the player, GameMode, or world may be
  // torn down before it returns. Capture weak pointers and re-resolve them
  // in the callback rather than holding raw pointers across the await.
  TWeakObjectPtr<APlayerController> WeakPlayerController(PlayerController);
  TWeakObjectPtr<AGameModeBase> WeakGameMode(GameMode);

  Sidecar->Emit(
    TEXT("realm:servers:player-auth:game-server-to-sidecar"),
    JsonObject,
    [PlayerId, WeakPlayerController, WeakGameMode](auto Response) {
      APlayerController *PlayerController = WeakPlayerController.Get();
      AGameModeBase *GameMode = WeakGameMode.Get();
      if (!IsValid(PlayerController) || !IsValid(GameMode)) {
        return;
      }
      TSharedPtr<FJsonObject> MessageStruct = Response[0]->AsObject();
      FString Error = MessageStruct->GetStringField(TEXT("error"));

      if (Error.IsEmpty()) {
        TSharedPtr<FJsonObject> Character =
          MessageStruct->GetObjectField(TEXT("character"));
        FString CharacterId = Character->GetStringField(TEXT("id"));
        FString CharacterName = Character->GetStringField(TEXT("name"));

        TSharedPtr<FJsonObject> Player =
          MessageStruct->GetObjectField(TEXT("player"));
        FString TempPlayerId = Player->GetStringField(TEXT("id"));

        URedwoodPlayerStateComponent *PlayerStateComponent =
          PlayerController->PlayerState
            ->FindComponentByClass<URedwoodPlayerStateComponent>();
        if (IsValid(PlayerStateComponent)) {
          UE_LOG(
            LogRedwood,
            Log,
            TEXT("Player joined as character %s"),
            *CharacterId
          );

          PlayerStateComponent->SetRedwoodPlayer(
            URedwoodCommonGameSubsystem::ParsePlayerData(Player)
          );

          // FORK(hollowed-oath) BEGIN: backend item-load leg. Upstream trunk here is a
          // single inline PlayerStateComponent->SetRedwoodCharacter(ParseCharacter(Character)).
          // The fork hoists the parse into ParsedCharacter so it can graft the item rows --
          // which the backend delivers as a SIBLING "items" field of the player-auth response,
          // NOT nested in "character" -- onto it BEFORE SetRedwoodCharacter fires
          // OnRedwoodCharacterUpdated. Merge must preserve that ordering (rows attached before the
          // set) so RedwoodPlayerStateCharacterUpdated has them in hand when it broadcasts.
          // (inventorySeq needs no such graft: unlike Items on THIS auth leg, the backend puts the
          // seq directly on the character object, so ParseCharacter above already picked it up. Note
          // ParseCharacter ALSO reads an inline "items" array, but only the backend character-LIST
          // response and offline saves place items there; the auth response nests nothing in
          // "character", so that inline read is a no-op here and this sibling graft is the only path
          // that populates Items on this leg. See the item-array contract in ParseCharacter.)
          FRedwoodCharacterBackend ParsedCharacter =
            URedwoodCommonGameSubsystem::ParseCharacter(Character);

          // Item rows ride this SAME player-auth response (a sibling field to "character", not
          // nested inside it -- see the backend's PlayerAuth.SidecarToRealm IResponse extension
          // that added "items" alongside "character"/"player"), so they are present before
          // SetRedwoodCharacter fires OnRedwoodCharacterUpdated below, instead of arriving later
          // via a separate realm:characters:items:load round trip.
          const TArray<TSharedPtr<FJsonValue>> *ItemsJsonArray = nullptr;
          if (MessageStruct->TryGetArrayField(TEXT("items"), ItemsJsonArray)) {
            ParsedCharacter.Items =
              URedwoodCommonGameSubsystem::ParseItemRecords(*ItemsJsonArray);
          }

          PlayerStateComponent->SetRedwoodCharacter(ParsedCharacter);
          // FORK(hollowed-oath) END
          PlayerStateComponent->SetServerReady();

          // The realm backend pushes party data to this server when the
          // player authenticates, but that push races with this auth
          // response; now that RedwoodPlayer.Id is set, reapply the
          // already-tracked parties so this player's PartyId is synced.
          URedwoodServerGameSubsystem *ServerSubsystem =
            GameMode->GetGameInstance()
              ->GetSubsystem<URedwoodServerGameSubsystem>();
          if (IsValid(ServerSubsystem)) {
            ServerSubsystem->UpdatePlayerStateComponentPartyIds();
          }

          if (PlayerStateComponent->bRanPostLogin) {
            GameMode->HandleStartingNewPlayer(PlayerController);
          }
        } else {
          UE_LOG(
            LogRedwood,
            Log,
            TEXT(
              "Player joined as character %s (player %s), but we're not using RedwoodPlayerStateComponent"
            ),
            *CharacterId,
            *TempPlayerId
          );
        }
      } else {
        UE_LOG(
          LogRedwood,
          Error,
          TEXT("Player failed to authenticate, kicking them now: %s"),
          *Error
        );
        if (IsValid(GameMode) && IsValid(GameMode->GameSession)) {
          GameMode->GameSession->KickPlayer(
            PlayerController, FText::FromString(Error)
          );
        } else {
          UE_LOG(
            LogRedwood,
            Error,
            TEXT(
              "Failed to kick player after authentication failure because GameMode or GameSession was invalid"
            )
          );
        }
      }
    }
  );
}
