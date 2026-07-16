// Copyright Incanta Games. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Pins the container
// wire-format contract for HollowedOath's per-container persistence channel (per-bag inventory):
// SerializeContainerRecords/ParseContainerRecords round-trip and ParseCharacter's inline
// "containers" reading (the offline/PIE leg) vs. its absence (the backend sidecar leg). WHY the
// fork needs it: the backend Container table and the offline character JSON are only
// interchangeable while they agree on the {containerId, kind, contents} shape these tests lock
// down. An upstream merge must keep this file with the URedwoodCommonGameSubsystem container APIs
// it exercises; if those APIs are renamed/removed upstream, update or drop this test in lockstep.
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCommonGameSubsystem.h"
#include "SIOJsonObject.h"
#include "Types/RedwoodTypesCharacters.h"

// Container rows round-trip through two different transports -- the backend's
// realm:characters:containers:upsert payload and the offline/PIE character JSON file -- and the
// two are only interchangeable for as long as they agree on the wire shape. These tests pin that
// shape (and the character object's "containers" field that carries it offline) without needing a
// backend, a world, or the disk.
//
// The namespace is named rather than anonymous, and referenced qualified rather than via a
// file-scope `using`: test .cpp files are concatenated into a shared translation unit by the
// unity build, where an anonymous namespace would merge with another test's and a `using` would
// leak into every file after this one in the blob.
namespace RedwoodContainerPersistenceTest {
  const FString TestContainerId = TEXT("11111111-2222-3333-4444-555555555555");
  constexpr uint8 TestContainerKind = 2;
  const FString TestContentsKey = TEXT("slotCount");
  constexpr int32 TestContentsValue = 3;

  USIOJsonObject *MakeTestContents() {
    USIOJsonObject *Contents = NewObject<USIOJsonObject>();
    TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
    Root->SetNumberField(TestContentsKey, TestContentsValue);
    Contents->SetRootObject(Root);
    return Contents;
  }

  FRedwoodContainerRecord MakeTestRecord() {
    FRedwoodContainerRecord Record;
    Record.ContainerId = TestContainerId;
    Record.Kind = TestContainerKind;
    Record.Contents = MakeTestContents();
    return Record;
  }

  // A character object carrying every field ParseCharacter reads unconditionally, so a test can
  // add just the part it cares about on top.
  TSharedPtr<FJsonObject> MakeMinimalCharacterObject() {
    TSharedPtr<FJsonObject> CharacterObject = MakeShareable(new FJsonObject);
    CharacterObject->SetStringField(TEXT("id"), TEXT("000"));
    CharacterObject->SetStringField(
      TEXT("createdAt"), FDateTime::UtcNow().ToIso8601()
    );
    CharacterObject->SetStringField(
      TEXT("updatedAt"), FDateTime::UtcNow().ToIso8601()
    );
    CharacterObject->SetStringField(TEXT("playerId"), TEXT("test-player"));
    CharacterObject->SetStringField(TEXT("name"), TEXT("Test Character"));
    return CharacterObject;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodContainerRecordRoundTripTest,
  "Redwood.Containers.RecordRoundTrip",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodContainerRecordRoundTripTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodContainerPersistenceTest;

  TArray<FRedwoodContainerRecord> Records;
  Records.Add(Fixture::MakeTestRecord());

  TArray<TSharedPtr<FJsonValue>> Serialized =
    URedwoodCommonGameSubsystem::SerializeContainerRecords(Records);

  TestEqual(TEXT("Serializes one row per record"), Serialized.Num(), 1);

  // These field names are the contract the backend's Container table reads and writes; changing
  // them silently breaks the offline leg's interchangeability with it.
  TSharedPtr<FJsonObject> Row = Serialized[0]->AsObject();
  TestTrue(TEXT("Row is an object"), Row.IsValid());
  TestEqual(
    TEXT("containerId field"),
    Row->GetStringField(TEXT("containerId")),
    Fixture::TestContainerId
  );
  TestEqual(
    TEXT("kind field"),
    (int32)Row->GetNumberField(TEXT("kind")),
    (int32)Fixture::TestContainerKind
  );
  TestTrue(TEXT("contents field present"), Row->HasField(TEXT("contents")));

  TArray<FRedwoodContainerRecord> Parsed =
    URedwoodCommonGameSubsystem::ParseContainerRecords(Serialized);

  TestEqual(TEXT("Parses one record per row"), Parsed.Num(), 1);
  if (Parsed.Num() != 1) {
    return false;
  }

  TestEqual(
    TEXT("ContainerId survives"), Parsed[0].ContainerId, Fixture::TestContainerId
  );
  TestEqual(
    TEXT("Kind survives"),
    (int32)Parsed[0].Kind,
    (int32)Fixture::TestContainerKind
  );
  TestTrue(
    TEXT("Contents survives"),
    Parsed[0].Contents != nullptr && Parsed[0].Contents->IsValid()
  );
  if (Parsed[0].Contents && Parsed[0].Contents->IsValid()) {
    TestEqual(
      TEXT("Contents payload survives"),
      (int32
      )Parsed[0].Contents->GetRootObject()->GetNumberField(Fixture::TestContentsKey
      ),
      Fixture::TestContentsValue
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodContainerRecordEmptyContentsTest,
  "Redwood.Containers.RecordEmptyContents",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodContainerRecordEmptyContentsTest::RunTest(const FString &Parameters
) {
  namespace Fixture = RedwoodContainerPersistenceTest;

  // A record whose Contents was never assigned still has to produce a readable row rather than
  // omitting "contents" and tripping the parser on the way back in.
  FRedwoodContainerRecord Record;
  Record.ContainerId = Fixture::TestContainerId;
  Record.Kind = Fixture::TestContainerKind;
  Record.Contents = nullptr;

  TArray<FRedwoodContainerRecord> Records;
  Records.Add(Record);

  TArray<TSharedPtr<FJsonValue>> Serialized =
    URedwoodCommonGameSubsystem::SerializeContainerRecords(Records);
  TestEqual(TEXT("Serializes the row"), Serialized.Num(), 1);
  if (Serialized.Num() != 1) {
    return false;
  }

  const TSharedPtr<FJsonObject> *ContentsObject = nullptr;
  TestTrue(
    TEXT("contents still written as an object"),
    Serialized[0]->AsObject()->TryGetObjectField(
      TEXT("contents"), ContentsObject
    )
  );

  TArray<FRedwoodContainerRecord> Parsed =
    URedwoodCommonGameSubsystem::ParseContainerRecords(Serialized);
  TestEqual(TEXT("Parses the row back"), Parsed.Num(), 1);
  if (Parsed.Num() == 1) {
    TestEqual(
      TEXT("ContainerId survives"),
      Parsed[0].ContainerId,
      Fixture::TestContainerId
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodParseCharacterReadsContainersTest,
  "Redwood.Containers.ParseCharacterReadsContainers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodParseCharacterReadsContainersTest::RunTest(const FString &Parameters
) {
  namespace Fixture = RedwoodContainerPersistenceTest;

  // The offline shape: rows inline on the character object, exactly as SaveCharacterToDisk writes
  // them. This is what makes a PIE load take the container path instead of degrading to the
  // legacy nonequipped-inventory split.
  TArray<FRedwoodContainerRecord> Records;
  Records.Add(Fixture::MakeTestRecord());

  TSharedPtr<FJsonObject> CharacterObject = Fixture::MakeMinimalCharacterObject();
  CharacterObject->SetArrayField(
    TEXT("containers"),
    URedwoodCommonGameSubsystem::SerializeContainerRecords(Records)
  );

  FRedwoodCharacterBackend Character =
    URedwoodCommonGameSubsystem::ParseCharacter(CharacterObject);

  TestEqual(
    TEXT("Containers parsed off the character"), Character.Containers.Num(), 1
  );
  if (Character.Containers.Num() == 1) {
    TestEqual(
      TEXT("ContainerId survives"),
      Character.Containers[0].ContainerId,
      Fixture::TestContainerId
    );
    TestEqual(
      TEXT("Kind survives"),
      (int32)Character.Containers[0].Kind,
      (int32)Fixture::TestContainerKind
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodParseCharacterWithoutContainersTest,
  "Redwood.Containers.ParseCharacterWithoutContainers",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodParseCharacterWithoutContainersTest::RunTest(
  const FString &Parameters
) {
  namespace Fixture = RedwoodContainerPersistenceTest;

  // The backend shape: no "containers" on the character object itself (the rows arrive as a
  // sibling of "character" in the player-auth response and are assigned after ParseCharacter
  // returns). Parsing must leave Containers empty rather than erroring, or the backend leg
  // regresses.
  FRedwoodCharacterBackend Character =
    URedwoodCommonGameSubsystem::ParseCharacter(
      Fixture::MakeMinimalCharacterObject()
    );

  TestEqual(TEXT("Containers left empty"), Character.Containers.Num(), 0);

  return true;
}
