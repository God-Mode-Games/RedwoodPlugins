// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. Replaces the
// quarantined ContainerPersistenceTest.cpp (RedwoodPlugins#17's per-container wire test, deleted
// alongside this file landing) now that persistence is a flat per-item channel. Pins two
// contracts HollowedOath's per-bag inventory depends on:
//   1. The FRedwoodItemRecord wire shape (SerializeItemRecords/ParseItemRecords) that the backend
//      Item table and the offline/PIE character JSON must agree on to stay interchangeable.
//   2. URedwoodCharacterComponent's item dirty channel + protocol-v2 single-flight/batchSeq
//      lifecycle (MarkItemsDirty/MarkItemsDeleted, TryBeginItemFlush/CompleteItemFlush,
//      SeedItemSeqFromCharacter) -- the generation-matched-ack and single-flight invariants are
//      the correctness core of the whole channel; see the FORK rationale blocks on
//      RedwoodCharacterComponent.h for what an upstream merge must preserve.
// An upstream merge must keep this file with the URedwoodCharacterComponent / item-record APIs it
// exercises; if those are renamed/removed upstream, update or drop this test in lockstep.
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "RedwoodCharacterComponent.h"
#include "RedwoodCommonGameSubsystem.h"
#include "RedwoodServerGameSubsystem.h"
#include "SIOJsonObject.h"
#include "Types/RedwoodTypesCharacters.h"

// The namespace is named rather than anonymous, and referenced qualified rather than via a
// file-scope `using`: test .cpp files are concatenated into a shared translation unit by the
// unity build, where an anonymous namespace would merge with another test's and a `using` would
// leak into every file after this one in the blob.
namespace RedwoodItemPersistenceTest {
  const FString TestItemIdA = TEXT("11111111-1111-1111-1111-111111111111");
  const FString TestItemIdB = TEXT("22222222-2222-2222-2222-222222222222");
  const FString TestParentId = TEXT("33333333-3333-3333-3333-333333333333");
  const FString TestDomain = TEXT("Bag");
  constexpr int32 TestSlot = 4;
  constexpr int32 TestQuantity = 7;
  const FString TestTemplateId = TEXT("Item_Test_Template");
  const FString TestAttributeKey = TEXT("durability");
  constexpr int32 TestAttributeValue = 42;

  USIOJsonObject *MakeTestAttributes() {
    USIOJsonObject *Attributes = NewObject<USIOJsonObject>();
    TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
    Root->SetNumberField(TestAttributeKey, TestAttributeValue);
    Attributes->SetRootObject(Root);
    return Attributes;
  }

  FRedwoodItemRecord MakeTestRecord() {
    FRedwoodItemRecord Record;
    Record.Id = TestItemIdA;
    Record.ParentId = TestParentId;
    Record.Domain = TestDomain;
    Record.Slot = TestSlot;
    Record.Quantity = TestQuantity;
    Record.TemplateId = TestTemplateId;
    Record.Attributes = MakeTestAttributes();
    return Record;
  }

  // A component instance stood up headless -- no owning actor, no BeginPlay. The dirty channel /
  // single-flight API under test (MarkItemsDirty, MarkItemsDeleted, TryBeginItemFlush,
  // CompleteItemFlush, SeedItemSeqFromCharacter) is pure component state and never touches
  // GetOwner(), so a bare NewObject is sufficient -- the same pattern the game's own component
  // tests use (e.g. NewObject<UInventoryComponent>() in InventoryTests.cpp).
  URedwoodCharacterComponent *MakeTestComponent() {
    URedwoodCharacterComponent *Component = NewObject<URedwoodCharacterComponent>();
    Component->bUseItems = true;
    return Component;
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemDirtyBumpAndCancelTest,
  "Redwood.Items.DirtyBumpAndCancel",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemDirtyBumpAndCancelTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  // Two marks on the same id coalesce into one entry whose generation is bumped, not duplicated.
  Component->MarkItemsDirty({Fixture::TestItemIdA});
  Component->MarkItemsDirty({Fixture::TestItemIdA});
  TestEqual(
    TEXT("One dirty entry after two marks"),
    Component->GetDirtyItemGenerations().Num(),
    1
  );
  const uint64 *GenerationAfterTwoMarks =
    Component->GetDirtyItemGenerations().Find(Fixture::TestItemIdA);
  TestTrue(TEXT("Id present in dirty map"), GenerationAfterTwoMarks != nullptr);
  if (GenerationAfterTwoMarks) {
    TestEqual(TEXT("Generation bumped to 2"), *GenerationAfterTwoMarks, (uint64)2);
  }

  // Marking a dirty id deleted cancels the dirty entry in favor of a pending deletion.
  Component->MarkItemsDeleted({Fixture::TestItemIdA});
  TestFalse(
    TEXT("No longer dirty after delete"),
    Component->GetDirtyItemGenerations().Contains(Fixture::TestItemIdA)
  );
  TestTrue(
    TEXT("Now pending delete"),
    Component->GetPendingDeletedItemGenerations().Contains(Fixture::TestItemIdA)
  );

  // Re-dirtying the same id cancels the pending deletion (delete-then-recreate within one flush
  // window) -- the vice-versa half of the coalesce contract.
  Component->MarkItemsDirty({Fixture::TestItemIdA});
  TestTrue(
    TEXT("Dirty again after re-mark"),
    Component->GetDirtyItemGenerations().Contains(Fixture::TestItemIdA)
  );
  TestFalse(
    TEXT("Pending delete cancelled by the re-mark"),
    Component->GetPendingDeletedItemGenerations().Contains(Fixture::TestItemIdA)
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemSingleFlightClaimReleaseTest,
  "Redwood.Items.SingleFlightClaimRelease",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemSingleFlightClaimReleaseTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  int64 FirstBatchSeq = 0;
  TestTrue(TEXT("First claim succeeds"), Component->TryBeginItemFlush(FirstBatchSeq));
  TestEqual(TEXT("First claim returns seq 1"), FirstBatchSeq, (int64)1);
  TestTrue(TEXT("Flush now in flight"), Component->IsItemFlushInFlight());

  // Exactly one item flush may be outstanding -- a second claim while one is already in flight
  // must fail and must not touch OutBatchSeq.
  int64 SecondBatchSeq = 0;
  TestFalse(
    TEXT("Second claim while in flight fails"), Component->TryBeginItemFlush(SecondBatchSeq)
  );
  TestEqual(TEXT("Second claim leaves OutBatchSeq untouched"), SecondBatchSeq, (int64)0);

  TArray<TPair<FString, uint64>> EmptySentList;
  Component->CompleteItemFlush(TEXT(""), 1, EmptySentList, EmptySentList);
  TestFalse(TEXT("Flight slot released"), Component->IsItemFlushInFlight());

  int64 ThirdBatchSeq = 0;
  TestTrue(
    TEXT("Next claim succeeds after release"), Component->TryBeginItemFlush(ThirdBatchSeq)
  );
  TestEqual(TEXT("Next claim returns seq 2"), ThirdBatchSeq, (int64)2);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemGenerationMatchedAckTest,
  "Redwood.Items.GenerationMatchedAck",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemGenerationMatchedAckTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  // Two ids dirtied before the flush is claimed: A will be re-dirtied WHILE the flush is in
  // flight, B is left untouched until the ack arrives.
  Component->MarkItemsDirty({Fixture::TestItemIdA, Fixture::TestItemIdB});

  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));

  // The flush captured generation 1 for both ids at send time.
  TArray<TPair<FString, uint64>> SentUpserts;
  SentUpserts.Add(TPair<FString, uint64>(Fixture::TestItemIdA, 1));
  SentUpserts.Add(TPair<FString, uint64>(Fixture::TestItemIdB, 1));
  TArray<TPair<FString, uint64>> SentDeletes;

  // A mutates again while the send is still in flight -- its generation moves to 2 before the ack
  // arrives.
  Component->MarkItemsDirty({Fixture::TestItemIdA});
  const uint64 *MidFlightGeneration =
    Component->GetDirtyItemGenerations().Find(Fixture::TestItemIdA);
  TestTrue(TEXT("A re-dirtied mid-flight"), MidFlightGeneration != nullptr);
  if (MidFlightGeneration) {
    TestEqual(TEXT("A generation bumped to 2 mid-flight"), *MidFlightGeneration, (uint64)2);
  }

  Component->CompleteItemFlush(TEXT(""), BatchSeq, SentUpserts, SentDeletes);

  // A's current generation (2) no longer matches what was sent (1), so the ack must not clear it
  // -- the re-dirty that raced the send is not allowed to be silently lost.
  TestTrue(
    TEXT("A survives the ack, still dirty"),
    Component->GetDirtyItemGenerations().Contains(Fixture::TestItemIdA)
  );
  const uint64 *SurvivingGeneration =
    Component->GetDirtyItemGenerations().Find(Fixture::TestItemIdA);
  if (SurvivingGeneration) {
    TestEqual(TEXT("A's newer generation preserved"), *SurvivingGeneration, (uint64)2);
  }

  // B was untouched during the flight, so its sent generation still matches current -- the ack
  // clears it.
  TestFalse(
    TEXT("B cleared by the ack"), Component->GetDirtyItemGenerations().Contains(Fixture::TestItemIdB)
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemErrorPathTest,
  "Redwood.Items.ErrorPath",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemErrorPathTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  Component->MarkItemsDirty({Fixture::TestItemIdA});

  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("Claim returns seq 1"), BatchSeq, (int64)1);

  TArray<TPair<FString, uint64>> SentUpserts;
  SentUpserts.Add(TPair<FString, uint64>(Fixture::TestItemIdA, 1));
  TArray<TPair<FString, uint64>> SentDeletes;

  // A network/backend failure: CommittedSeq (0) trails what was sent (1), so this is a plain
  // retry -- not a replay/fence the backend has already committed past.
  Component->CompleteItemFlush(TEXT("boom"), 0, SentUpserts, SentDeletes);

  TestFalse(
    TEXT("Flight slot released despite the error"), Component->IsItemFlushInFlight()
  );
  TestTrue(
    TEXT("Dirty entry survives an error"),
    Component->GetDirtyItemGenerations().Contains(Fixture::TestItemIdA)
  );
  TestEqual(TEXT("Committed seq did not move"), Component->GetLastCommittedItemSeq(), (int64)0);

  // NextBatchSeq is unchanged by a plain error -- the next claim re-uses the same seq rather than
  // skipping ahead, so the retry lines up with what the backend still expects.
  int64 RetryBatchSeq = 0;
  TestTrue(TEXT("Retry claim succeeds"), Component->TryBeginItemFlush(RetryBatchSeq));
  TestEqual(TEXT("Retry re-uses seq 1"), RetryBatchSeq, (int64)1);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemReplayResyncTest,
  "Redwood.Items.ReplayResync",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemReplayResyncTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  // Seed so the claimed seq is 3, matching the pinned "sent seq was 3" scenario.
  Component->SeedItemSeqFromCharacter(2);

  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("Claim returns seq 3"), BatchSeq, (int64)3);

  TArray<TPair<FString, uint64>> EmptySentList;

  // The backend acks success but reports a committed seq (5) ahead of what this flush sent (3) --
  // e.g. it had already advanced past a replayed/duplicate batch. The ack must resync to the
  // backend's authoritative value rather than trusting only the seq this flush stamped.
  Component->CompleteItemFlush(TEXT(""), 5, EmptySentList, EmptySentList);

  TestEqual(
    TEXT("Committed seq resyncs to the backend's value"),
    Component->GetLastCommittedItemSeq(),
    (int64)5
  );

  int64 NextBatchSeqAfterResync = 0;
  TestTrue(TEXT("Next claim succeeds"), Component->TryBeginItemFlush(NextBatchSeqAfterResync));
  TestEqual(
    TEXT("Next batch seq continues from the resynced value"), NextBatchSeqAfterResync, (int64)6
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemSeedTest,
  "Redwood.Items.Seed",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemSeedTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;
  URedwoodCharacterComponent *Component = Fixture::MakeTestComponent();

  // Primes the committed-seq/next-batchSeq state from a freshly-loaded character's InventorySeq
  // so the first flush of a session continues the backend's sequence instead of restarting at 1.
  Component->SeedItemSeqFromCharacter(7);
  TestEqual(TEXT("Seeded committed seq"), Component->GetLastCommittedItemSeq(), (int64)7);

  int64 BatchSeq = 0;
  TestTrue(TEXT("Claim succeeds after seeding"), Component->TryBeginItemFlush(BatchSeq));
  TestEqual(TEXT("Next claim continues the seeded sequence"), BatchSeq, (int64)8);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemRecordRoundTripTest,
  "Redwood.Items.RecordRoundTrip",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemRecordRoundTripTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;

  TArray<FRedwoodItemRecord> Records;
  Records.Add(Fixture::MakeTestRecord());

  TArray<TSharedPtr<FJsonValue>> Serialized =
    URedwoodCommonGameSubsystem::SerializeItemRecords(Records);
  TestEqual(TEXT("Serializes one row per record"), Serialized.Num(), 1);
  if (Serialized.Num() != 1) {
    return false;
  }

  // These field names are the contract the backend's Item table reads and writes; changing them
  // silently breaks the offline leg's interchangeability with it.
  TSharedPtr<FJsonObject> Row = Serialized[0]->AsObject();
  TestTrue(TEXT("Row is an object"), Row.IsValid());
  TestEqual(TEXT("id field"), Row->GetStringField(TEXT("id")), Fixture::TestItemIdA);
  TestEqual(
    TEXT("parentId field"), Row->GetStringField(TEXT("parentId")), Fixture::TestParentId
  );
  TestEqual(TEXT("domain field"), Row->GetStringField(TEXT("domain")), Fixture::TestDomain);
  TestEqual(
    TEXT("slot field"), (int32)Row->GetNumberField(TEXT("slot")), Fixture::TestSlot
  );
  TestEqual(
    TEXT("quantity field"), (int32)Row->GetNumberField(TEXT("quantity")), Fixture::TestQuantity
  );
  TestEqual(
    TEXT("templateId field"), Row->GetStringField(TEXT("templateId")), Fixture::TestTemplateId
  );
  TestTrue(TEXT("attributes field present"), Row->HasField(TEXT("attributes")));

  TArray<FRedwoodItemRecord> Parsed =
    URedwoodCommonGameSubsystem::ParseItemRecords(Serialized);
  TestEqual(TEXT("Parses one record per row"), Parsed.Num(), 1);
  if (Parsed.Num() != 1) {
    return false;
  }

  TestEqual(TEXT("Id survives"), Parsed[0].Id, Fixture::TestItemIdA);
  TestEqual(TEXT("ParentId survives"), Parsed[0].ParentId, Fixture::TestParentId);
  TestEqual(TEXT("Domain survives"), Parsed[0].Domain, Fixture::TestDomain);
  TestEqual(TEXT("Slot survives"), Parsed[0].Slot, Fixture::TestSlot);
  TestEqual(TEXT("Quantity survives"), Parsed[0].Quantity, Fixture::TestQuantity);
  TestEqual(TEXT("TemplateId survives"), Parsed[0].TemplateId, Fixture::TestTemplateId);
  TestTrue(
    TEXT("Attributes survives"),
    Parsed[0].Attributes != nullptr && Parsed[0].Attributes->IsValid()
  );
  if (Parsed[0].Attributes && Parsed[0].Attributes->IsValid()) {
    TestEqual(
      TEXT("Attribute payload survives"),
      (int32
      )Parsed[0].Attributes->GetRootObject()->GetNumberField(Fixture::TestAttributeKey),
      Fixture::TestAttributeValue
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemRecordRoundTripNullFieldsTest,
  "Redwood.Items.RecordRoundTripNullFields",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodItemRecordRoundTripNullFieldsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;

  // A root item (empty ParentId) with no Attributes ever assigned -- the two edge cases the wire
  // contract has to special-case: ParentId="" must serialize as JSON null (not an empty string,
  // which the backend's nullable parentId column would treat differently), and a null Attributes
  // pointer must still produce a readable "attributes" object rather than omitting the field.
  FRedwoodItemRecord Record;
  Record.Id = Fixture::TestItemIdA;
  Record.ParentId = TEXT("");
  Record.Domain = Fixture::TestDomain;
  Record.Slot = Fixture::TestSlot;
  Record.Quantity = Fixture::TestQuantity;
  Record.TemplateId = Fixture::TestTemplateId;
  Record.Attributes = nullptr;

  TArray<FRedwoodItemRecord> Records;
  Records.Add(Record);

  TArray<TSharedPtr<FJsonValue>> Serialized =
    URedwoodCommonGameSubsystem::SerializeItemRecords(Records);
  TestEqual(TEXT("Serializes the row"), Serialized.Num(), 1);
  if (Serialized.Num() != 1) {
    return false;
  }

  TSharedPtr<FJsonObject> Row = Serialized[0]->AsObject();
  const TSharedPtr<FJsonValue> ParentIdValue = Row->TryGetField(TEXT("parentId"));
  TestTrue(TEXT("parentId present"), ParentIdValue.IsValid());
  if (ParentIdValue.IsValid()) {
    TestTrue(TEXT("Empty ParentId serializes as JSON null"), ParentIdValue->IsNull());
  }

  const TSharedPtr<FJsonObject> *AttributesObject = nullptr;
  TestTrue(
    TEXT("attributes still written as an object"),
    Row->TryGetObjectField(TEXT("attributes"), AttributesObject)
  );

  TArray<FRedwoodItemRecord> Parsed =
    URedwoodCommonGameSubsystem::ParseItemRecords(Serialized);
  TestEqual(TEXT("Parses the row back"), Parsed.Num(), 1);
  if (Parsed.Num() == 1) {
    TestEqual(
      TEXT("Empty ParentId round-trips to empty string"), Parsed[0].ParentId, FString()
    );
    TestTrue(
      TEXT("Null Attributes round-trips to a non-null empty object"),
      Parsed[0].Attributes != nullptr && Parsed[0].Attributes->IsValid()
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodItemEmitEnvelopeTest,
  "Redwood.Items.EmitEnvelopes",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

// Pins the wire ENVELOPE of the three item emits (flush / migrate / trade). The sidecar validates
// each request against its full schema -- including id: string().required(), the field the sidecar
// itself stamps -- BEFORE stamping, so an envelope without a placeholder id never reaches the
// realm ("id is a required field"). This layer shipped untested once and was live-broken for all
// three routes on the first deployed channel; these assertions make that unrepresentable.
bool FRedwoodItemEmitEnvelopeTest::RunTest(const FString &Parameters) {
  const FString CharacterId = TEXT("character-1");

  // A payload id must be a present, non-empty STRING (JSON null also fails the sidecar's schema).
  auto TestEnvelopeId = [this](
                          const TCHAR *Label, const TSharedPtr<FJsonObject> &Payload
                        ) {
    FString EnvelopeId;
    TestTrue(
      FString::Printf(TEXT("%s: envelope has a string id"), Label),
      Payload->TryGetStringField(TEXT("id"), EnvelopeId)
    );
    TestFalse(
      FString::Printf(TEXT("%s: envelope id is non-empty"), Label),
      EnvelopeId.IsEmpty()
    );
  };

  // --- flush ---
  {
    TArray<TSharedPtr<FJsonValue>> Upserts;
    Upserts.Add(MakeShareable(new FJsonValueObject(
      URedwoodCommonGameSubsystem::SerializeItemRecord(
        RedwoodItemPersistenceTest::MakeTestRecord()
      )
    )));
    TArray<TSharedPtr<FJsonValue>> Deletes;

    TSharedPtr<FJsonObject> Payload =
      URedwoodServerGameSubsystem::BuildItemsFlushPayload(
        CharacterId, 7, Upserts, Deletes
      );
    TestEnvelopeId(TEXT("flush"), Payload);
    TestEqual(
      TEXT("flush: characterId"),
      Payload->GetStringField(TEXT("characterId")),
      CharacterId
    );
    TestEqual(
      TEXT("flush: batchSeq"), Payload->GetNumberField(TEXT("batchSeq")), 7.0
    );
    TestTrue(
      TEXT("flush: upserts array present"),
      Payload->HasTypedField<EJson::Array>(TEXT("upserts"))
    );
    TestTrue(
      TEXT("flush: deletes array present"),
      Payload->HasTypedField<EJson::Array>(TEXT("deletes"))
    );
  }

  // (The blob->row migrate envelope was deleted -- row-per-item is the native model now, so there
  // is no migrate builder to pin.)

  // --- trade ---
  {
    FRedwoodTradeRootPlacement RootPlacement;
    RootPlacement.Id = RedwoodItemPersistenceTest::TestItemIdA;
    RootPlacement.Domain = TEXT("general");
    RootPlacement.Slot = 3;
    // Empty ParentId: must serialize as JSON null (root placement), not "".
    FRedwoodTradeRootPlacement ChildPlacement;
    ChildPlacement.Id = RedwoodItemPersistenceTest::TestItemIdB;
    ChildPlacement.Domain = TEXT("content");
    ChildPlacement.Slot = 0;
    ChildPlacement.ParentId = RedwoodItemPersistenceTest::TestParentId;

    TSharedPtr<FJsonObject> Payload =
      URedwoodServerGameSubsystem::BuildItemsTradePayload(
        TEXT("character-from"), TEXT("character-to"), {RootPlacement, ChildPlacement}
      );
    TestEnvelopeId(TEXT("trade"), Payload);
    TestEqual(
      TEXT("trade: fromCharacterId"),
      Payload->GetStringField(TEXT("fromCharacterId")),
      FString(TEXT("character-from"))
    );
    TestEqual(
      TEXT("trade: toCharacterId"),
      Payload->GetStringField(TEXT("toCharacterId")),
      FString(TEXT("character-to"))
    );
    const TArray<TSharedPtr<FJsonValue>> *Placements = nullptr;
    TestTrue(
      TEXT("trade: rootPlacements present"),
      Payload->TryGetArrayField(TEXT("rootPlacements"), Placements)
    );
    if (Placements && Placements->Num() == 2) {
      const TSharedPtr<FJsonObject> *Root = nullptr;
      const TSharedPtr<FJsonObject> *Child = nullptr;
      if ((*Placements)[0]->TryGetObject(Root) &&
          (*Placements)[1]->TryGetObject(Child)) {
        TSharedPtr<FJsonValue> RootParent = (*Root)->TryGetField(TEXT("parentId"));
        TestTrue(
          TEXT("trade: empty ParentId serializes as JSON null"),
          RootParent.IsValid() && RootParent->IsNull()
        );
        TestEqual(
          TEXT("trade: content-child ParentId survives"),
          (*Child)->GetStringField(TEXT("parentId")),
          RedwoodItemPersistenceTest::TestParentId
        );
      }
    } else {
      TestEqual(
        TEXT("trade: both placements serialized"),
        Placements ? Placements->Num() : 0,
        2
      );
    }
  }

  return true;
}

// Pins that ParseCharacter reads an INLINE "items" array off the character object -- the placement
// the offline/PIE-disk save uses AND the one the backend character-LIST response now uses for the
// character-select preview (equipment/cosmetic/socket rows). The player-AUTH leg instead delivers
// items as a sibling of "character" grafted on in RedwoodGameModeComponent; that path is exercised
// live, but this inline read is the one both offline saves and the list preview depend on. See the
// item-array contract comment in ParseCharacter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodParseCharacterInlineItemsTest,
  "Redwood.Items.ParseCharacterInlineItems",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodParseCharacterInlineItemsTest::RunTest(const FString &Parameters) {
  namespace Fixture = RedwoodItemPersistenceTest;

  TSharedPtr<FJsonObject> CharacterObj = MakeShareable(new FJsonObject);
  CharacterObj->SetStringField(TEXT("id"), TEXT("character-1"));
  CharacterObj->SetStringField(TEXT("playerId"), TEXT("player-1"));
  CharacterObj->SetStringField(TEXT("name"), TEXT("Test Character"));
  CharacterObj->SetStringField(TEXT("createdAt"), TEXT("2026-07-20T00:00:00.000Z"));
  CharacterObj->SetStringField(TEXT("updatedAt"), TEXT("2026-07-20T00:00:00.000Z"));

  // Inline "items" array, exactly the shape the backend character-LIST response carries.
  TArray<FRedwoodItemRecord> Records;
  Records.Add(Fixture::MakeTestRecord());
  CharacterObj->SetArrayField(
    TEXT("items"), URedwoodCommonGameSubsystem::SerializeItemRecords(Records)
  );
  CharacterObj->SetNumberField(TEXT("inventorySeq"), 5);

  FRedwoodCharacterBackend Parsed =
    URedwoodCommonGameSubsystem::ParseCharacter(CharacterObj);

  TestEqual(TEXT("Inline items parsed onto the struct"), Parsed.Items.Num(), 1);
  if (Parsed.Items.Num() == 1) {
    TestEqual(
      TEXT("Inline item id survives"), Parsed.Items[0].Id, Fixture::TestItemIdA
    );
    TestEqual(
      TEXT("Inline item templateId survives"),
      Parsed.Items[0].TemplateId,
      Fixture::TestTemplateId
    );
  }
  TestEqual(
    TEXT("InventorySeq fence parsed"), Parsed.InventorySeq, (int64)5
  );

  // A character object with NO "items" field must leave Items empty (auth-leg / no-inline-rows
  // tolerance -- the field is simply absent there).
  TSharedPtr<FJsonObject> NoItemsObj = MakeShareable(new FJsonObject);
  NoItemsObj->SetStringField(TEXT("id"), TEXT("character-2"));
  NoItemsObj->SetStringField(TEXT("playerId"), TEXT("player-1"));
  NoItemsObj->SetStringField(TEXT("name"), TEXT("No Items"));
  NoItemsObj->SetStringField(TEXT("createdAt"), TEXT("2026-07-20T00:00:00.000Z"));
  NoItemsObj->SetStringField(TEXT("updatedAt"), TEXT("2026-07-20T00:00:00.000Z"));

  FRedwoodCharacterBackend ParsedNoItems =
    URedwoodCommonGameSubsystem::ParseCharacter(NoItemsObj);
  TestEqual(
    TEXT("Absent items field leaves Items empty"), ParsedNoItems.Items.Num(), 0
  );

  return true;
}
