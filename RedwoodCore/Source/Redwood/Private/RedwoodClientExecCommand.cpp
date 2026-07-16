// Copyright Incanta Games. All rights reserved.

#include "RedwoodClientExecCommand.h"
// FORK(hollowed-oath): NetCore PushModel include for the push-model rep branch below.
#include "Net/Core/PushModel/PushModel.h"
#include "Net/UnrealNetwork.h"

void ARedwoodClientExecCommand::BeginPlay() {
  Super::BeginPlay();

  UWorld *World = GetWorld();

  if (IsValid(World) && World->GetNetMode() == ENetMode::NM_Client && !Command.IsEmpty()) {
    GetWorld()->Exec(World, *Command);
  }
}

void ARedwoodClientExecCommand::GetLifetimeReplicatedProps(
  TArray<FLifetimeProperty> &OutLifetimeProps
) const {
  Super::GetLifetimeReplicatedProps(OutLifetimeProps);

  // FORK(hollowed-oath) BEGIN: push-model replication for Command (upstream is a bare
  // DOREPLIFETIME). No dirty-mark is needed anywhere because Command never mutates after spawn
  // (see note below) -- it rides initial replication. Merge must preserve both legs.
  // Command is set once at deferred spawn (before FinishSpawningActor), so its
  // value rides the initial replication; no MARK_PROPERTY_DIRTY is needed because
  // it never mutates after the actor begins replicating.
#if WITH_PUSH_MODEL
  if (IS_PUSH_MODEL_ENABLED()) {
    FDoRepLifetimeParams Params;
    Params.bIsPushBased = true;
    DOREPLIFETIME_WITH_PARAMS_FAST(ARedwoodClientExecCommand, Command, Params);
  } else
#endif
  {
    DOREPLIFETIME(ARedwoodClientExecCommand, Command);
  }
  // FORK(hollowed-oath) END
}
