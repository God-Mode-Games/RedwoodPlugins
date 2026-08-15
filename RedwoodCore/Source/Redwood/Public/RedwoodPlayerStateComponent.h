// Copyright Incanta LLC. All rights reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "Types/RedwoodTypes.h"

#include "RedwoodPlayerStateComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRedwoodPlayerStateUpdated);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRedwoodPlayerTransferring);

// FORK(hollowed-oath): transfer-abort notifications, plus the server-side
// counterparts of the two client events. See AbortTransferring below.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
  FOnRedwoodPlayerTransferAborted, FString, Error, FString, Reason
);
DECLARE_MULTICAST_DELEGATE(FOnRedwoodPlayerTransferringServer);
DECLARE_MULTICAST_DELEGATE_TwoParams(
  FOnRedwoodPlayerTransferAbortedServer,
  const FString & /* Error */,
  const FString & /* Reason */
);

UCLASS(
  BlueprintType,
  Blueprintable,
  ClassGroup = (Redwood),
  meta = (BlueprintSpawnableComponent)
)
class REDWOOD_API URedwoodPlayerStateComponent : public UActorComponent {
  GENERATED_BODY()

public:
  URedwoodPlayerStateComponent(const FObjectInitializer &ObjectInitializer);

  // NOT AVAILABLE ON CLIENTS
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodPlayerData RedwoodPlayer;
  // NOT AVAILABLE ON CLIENTS
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FRedwoodCharacterBackend RedwoodCharacter;

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void MarkCharacterDataDirty() {
    bCharacterDataDirty = true;
  }

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  bool IsCharacterDataDirty() const {
    return bCharacterDataDirty;
  }

  //~ Begin UActorComponent Interface
  virtual void TickComponent(
    float DeltaTime,
    enum ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction
  ) override;
  virtual void GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty> &OutLifetimeProps
  ) const override;
  //~ End UActorComponent Interface

  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Redwood")
  bool bFollowPawn = false;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  bool bClientReady = false;

  UPROPERTY(BlueprintReadOnly, Category = "Redwood")
  bool bServerReady = false;

  UFUNCTION(
    BlueprintCallable,
    Server,
    Reliable,
    WithValidation,
    Category = "Redwood|PlayerState"
  )
  void SetClientReady();

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  void SetServerReady();

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetRedwoodPlayer(FRedwoodPlayerData InRedwoodPlayer);

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetRedwoodCharacter(FRedwoodCharacterBackend InRedwoodCharacter);

  // The id of the party this player is in; empty if not in a party.
  // Set on the server when the player logs in and replicated to all
  // clients. It does NOT automatically update if the player's party
  // changes while they're in this server; the server can refresh it
  // via URedwoodServerGameSubsystem::GetPartyByPlayerId + SetPartyId.
  UPROPERTY(
    BlueprintReadOnly, ReplicatedUsing = OnRep_PartyId, Category = "Redwood"
  )
  FString PartyId;

  UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Redwood")
  void SetPartyId(const FString &InPartyId);

  /**
   * Returns the URedwoodPlayerStateComponents of all players in this
   * player's party, based on matching replicated PartyId values; usable
   * on both the server and clients. This component is always included
   * when bExcludeSelf is false, even if the player isn't in a party.
   */
  UFUNCTION(BlueprintCallable, Category = "Redwood")
  TArray<URedwoodPlayerStateComponent *> GetPartyMemberPlayerStateComponents(
    bool bExcludeSelf = false
  ) const;

  UFUNCTION(BlueprintCallable, Category = "Redwood")
  bool GetSpawnData(FTransform &Transform, FRotator &ControlRotation);

  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnRedwoodCharacterUpdated;

  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnRedwoodPlayerUpdated;

  // Broadcast on the server and on all clients when PartyId changes.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerStateUpdated OnPartyIdChanged;

  bool bTransferring = false;

  // FORK(hollowed-oath): the id the realm gave the transfer that is now in
  // flight, taken from the transfer-zone answer. Server-only and never
  // replicated: only the game server matches reports against it.
  // InitTransferring and AbortTransferring both clear it, so no transfer can
  // keep the name of an earlier one.
  FString ActiveTransferId;

  /**
   * FORK(hollowed-oath): True when a failure report that names TransferId
   * belongs to the transfer in flight. An empty id on EITHER side counts as
   * a match: an older backend sends no id at all, and a failure can reach
   * this server before the answer that carries the id. Only two different
   * non-empty ids are a mismatch. The bTransferring latch stays the real
   * guard; this only drops a report for an earlier transfer.
   */
  bool MatchesActiveTransfer(const FString &TransferId) const {
    return TransferId.IsEmpty() || ActiveTransferId.IsEmpty() ||
      TransferId == ActiveTransferId;
  }

  /**
   * Server-only entry point that begins a zone transfer for this player.
   * Marks the component as transferring and notifies the owning client
   * via Client_OnTransferring, which broadcasts OnTransferring locally.
   * Called from URedwoodServerGameSubsystem's TravelPlayerToZone* paths
   * in place of setting bTransferring directly.
   * FORK(hollowed-oath): also clears ActiveTransferId and broadcasts
   * OnTransferringStartedServer.
   */
  void InitTransferring();

  /**
   * Reliable RPC delivered only to this PlayerState's owning client.
   * Broadcasts OnTransferring so C++/Blueprint listeners on the owning
   * client can react (e.g. show a loading screen) before the travel.
   */
  UFUNCTION(Client, Reliable, Category = "Redwood|PlayerState")
  void Client_OnTransferring();

  // Broadcast on the owning client when a zone transfer begins. Bind in
  // C++ or Blueprints. Only fires on the owning client (see
  // Client_OnTransferring); it does not fire on the server or other
  // clients.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerTransferring OnTransferring;

  /**
   * FORK(hollowed-oath): Reliable RPC delivered only to this PlayerState's
   * owning client. Broadcasts OnTransferAborted so the client can remove
   * the loading screen that Client_OnTransferring put up.
   */
  UFUNCTION(Client, Reliable, Category = "Redwood|PlayerState")
  void Client_OnTransferAborted(const FString &Error, const FString &Reason);

  // FORK(hollowed-oath): Broadcast on the owning client when a zone
  // transfer stops before it completes. Only fires on the owning client
  // (see Client_OnTransferAborted); it does not fire on the server or
  // other clients.
  UPROPERTY(BlueprintAssignable, Category = "Events")
  FOnRedwoodPlayerTransferAborted OnTransferAborted;

  // FORK(hollowed-oath): server-side counterparts of OnTransferring and
  // OnTransferAborted. The two events above only fire on the owning
  // client, but game C++ on the server must also know when a transfer
  // starts and when it stops. Native (not dynamic) because they are
  // server-only and not for Blueprints; bind them per component.
  FOnRedwoodPlayerTransferringServer OnTransferringStartedServer;
  FOnRedwoodPlayerTransferAbortedServer OnTransferAbortedServer;

  // FORK(hollowed-oath): rollback for a transfer that does not complete.
  // InitTransferring runs BEFORE the TravelPlayerToZone* functions test the
  // sidecar, and upstream has NO path that clears the flag afterwards — the
  // player then stays marked as transferring for the rest of the session,
  // which also disables linkdead pawn retention and lastLocation
  // persistence in the game project.
  //
  // An upstream merge must keep the three callers, all in
  // RedwoodServerGameSubsystem.cpp, which own their own rules:
  //   1. the sidecar-down early returns in TravelPlayerToZone*;
  //   2. HandleTransferZoneResponse (which failures roll back and which
  //      still kick);
  //   3. HandleTransferFailedEvent (the realm's later report).
  //
  // Reason is a short token that the game maps to its own failure type;
  // Error stays the human-readable text. The transfer-failed payload
  // carries the backend's own kebab-case tokens ("zone-not-configured",
  // "no-shard-available", "zone-start-timeout",
  // "character-not-transferable", "proxy-not-found"), passed through
  // unchanged. Two failures never reach the backend, so this plugin names
  // them:
  //   "sidecar-down"    the sidecar is not connected, so the request never
  //                     left this game server;
  //   "realm-rejected"  the sidecar answered with an error that it marks
  //                     safe to roll back.
  // Keep both plugin tokens stable; the game matches on them.

  /**
   * FORK(hollowed-oath): Server-only. Clears bTransferring and
   * ActiveTransferId, broadcasts OnTransferAbortedServer, and tells the
   * owning client with Client_OnTransferAborted, so the game can remove the
   * loading screen that the transfer put up.
   * Does nothing when the player is not transferring, so one start can
   * never produce two aborts. The callers gate too, only for better logs.
   * Reason has no default: a new caller must say which failure this is.
   */
  void AbortTransferring(const FString &Error, const FString &Reason);

  void ClearDirtyFlags() {
    bCharacterDataDirty = false;
  }

  bool bRanPostLogin = false;

private:
  UFUNCTION()
  void OnRep_PartyId();

  TWeakObjectPtr<APlayerState> OwnerPlayerState = nullptr;

  bool bCharacterDataDirty = false;
};
