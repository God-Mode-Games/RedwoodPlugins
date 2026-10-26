// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"
#include "RedwoodModule.h"

#include "RedwoodTypesEvents.generated.h"

// Where a realm event goes. A Topic event goes to every session on one
// Bifrost topic; a Session event goes to one player's session only.
UENUM(BlueprintType)
enum class ERedwoodRealmEventKind : uint8 { Topic, Session };

// The chat category the line arrives under. The client picks the channel it
// shows the line in from this word, so it is part of the Bifrost contract.
UENUM(BlueprintType)
enum class ERedwoodRealmEventCategory : uint8 { All, Party, Whisper, System };

// A chat line the game server publishes through the realm to Bifrost. See
// URedwoodServerGameSubsystem::PublishRealmEvent.
//
// Template and Args are the line as the client renders it: the template
// holds the words, the args the values the client fills in, so the client
// owns the language and the server never sends a finished sentence. SenderName
// is optional; a line with no sender is the server's own voice.
//
// Build one through the three helpers below where you can. They hold the
// topic strings in one place: Bifrost, the client's chat routing and this
// struct must all spell `r:<realmId>/all`, `r:<realmId>/char:<characterId>`
// and `r:<realmId>/party:<partyId>` the same way, and a drift in any of them
// is a line nobody receives.
USTRUCT(BlueprintType)
struct REDWOOD_API FRedwoodRealmEvent {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  ERedwoodRealmEventKind Kind = ERedwoodRealmEventKind::Topic;

  // Set for a Topic event; left empty for a Session event.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Topic;

  // Set for a Session event; left empty for a Topic event.
  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString SessionId;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  ERedwoodRealmEventCategory Category = ERedwoodRealmEventCategory::System;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString Template;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  TArray<FString> Args;

  UPROPERTY(BlueprintReadWrite, Category = "Redwood")
  FString SenderName;

  // The parts of a realm topic. Pinned by a test, because Bifrost and the
  // game's chat routing each keep their own copy of these strings.
  static constexpr const TCHAR *RealmTopicPrefix = TEXT("r:");
  static constexpr const TCHAR *AllTopicWord = TEXT("all");
  static constexpr const TCHAR *CharacterTopicWord = TEXT("char:");
  static constexpr const TCHAR *PartyTopicWord = TEXT("party:");

  // The three helpers refuse an id that is empty or holds a `/`: the topic
  // would then name nobody, or a different topic than the caller meant, and
  // Bifrost would store the line under it for good. They log a warning and
  // leave the Topic empty, so CheckRealmEvent refuses the event and nothing
  // is sent. A warning, not an ensure: an ensure's report is several log
  // lines the automation framework counts as errors one by one.

  // A line for every player on the realm, under the "all" category.
  static FRedwoodRealmEvent RealmWide(
    const FString &RealmId, const FString &InTemplate
  ) {
    return ForTopic(
      RealmTopic(RealmId, AllTopicWord, true),
      ERedwoodRealmEventCategory::All,
      InTemplate
    );
  }

  // A line for one character, wherever it is in play, as a system line: the
  // server speaks to the player, it does not whisper as one.
  static FRedwoodRealmEvent ForCharacter(
    const FString &RealmId,
    const FString &CharacterId,
    const FString &InTemplate
  ) {
    return ForTopic(
      RealmTopic(
        RealmId, CharacterTopicWord + CharacterId, IsTopicId(CharacterId)
      ),
      ERedwoodRealmEventCategory::System,
      InTemplate
    );
  }

  // A line for every member of one party, on every game server.
  static FRedwoodRealmEvent ForParty(
    const FString &RealmId, const FString &PartyId, const FString &InTemplate
  ) {
    return ForTopic(
      RealmTopic(RealmId, PartyTopicWord + PartyId, IsTopicId(PartyId)),
      ERedwoodRealmEventCategory::Party,
      InTemplate
    );
  }

private:
  // An id is one topic part: not empty, and no `/`, which is the separator.
  static bool IsTopicId(const FString &Id) {
    return !Id.IsEmpty() && !Id.Contains(TEXT("/"));
  }

  // Empty for a bad id, so the event fails CheckRealmEvent and nothing is
  // sent. The caller judges its own id; the realm id is judged here.
  static FString RealmTopic(
    const FString &RealmId, const FString &Suffix, bool bIdIsGood
  ) {
    if (!IsTopicId(RealmId) || !bIdIsGood) {
      UE_LOG(
        LogRedwood,
        Warning,
        TEXT("Refused the realm topic '%s' for realm '%s'"),
        *Suffix,
        *RealmId
      );
      return FString();
    }
    return RealmTopicPrefix + RealmId + TEXT("/") + Suffix;
  }

  static FRedwoodRealmEvent ForTopic(
    const FString &InTopic,
    ERedwoodRealmEventCategory InCategory,
    const FString &InTemplate
  ) {
    FRedwoodRealmEvent Event;
    Event.Kind = ERedwoodRealmEventKind::Topic;
    Event.Topic = InTopic;
    Event.Category = InCategory;
    Event.Template = InTemplate;
    return Event;
  }
};
