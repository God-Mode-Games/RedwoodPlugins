// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins how URedwoodClientInterface::ParseRealm reads the realm version that
// the client uses to offer only realms of its own build:
//   1. A version the director sends is kept as-is.
//   2. An older director sends no version key; the realm parses with an
//      empty version and its other fields intact.
//   3. A null version is also empty, so it cannot match a real commit.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "RedwoodClientInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace {
  const TCHAR *const RealmFieldsWithoutVersion = TEXT(
    "\"id\": \"realm-a\", \"createdAt\": \"2024-01-01T00:00:00.000Z\", "
    "\"updatedAt\": \"2024-01-02T11:42:24.000Z\", \"name\": \"Realm A\", "
    "\"uri\": \"ws://127.0.0.1:3011\", \"listed\": true, \"secret\": \"\""
  );

  FRedwoodRealm ParseRealmJson(const FString &ExtraFields) {
    const FString Json =
      FString::Printf(TEXT("{%s%s}"), RealmFieldsWithoutVersion, *ExtraFields);
    TSharedPtr<FJsonObject> Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
    check(Object.IsValid());
    return URedwoodClientInterface::ParseRealm(Object);
  }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodRealmVersionParseTest,
  "Redwood.Realms.ParseVersion",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodRealmVersionParseTest::RunTest(const FString &Parameters) {
  const FString Commit = TEXT("0123456789abcdef0123456789abcdef01234567");

  const FRedwoodRealm WithVersion =
    ParseRealmJson(FString::Printf(TEXT(", \"version\": \"%s\""), *Commit));
  TestEqual(TEXT("version is kept"), WithVersion.Version, Commit);

  const FRedwoodRealm WithoutVersion = ParseRealmJson(FString());
  TestEqual(
    TEXT("missing version is empty"), WithoutVersion.Version, FString()
  );
  TestEqual(TEXT("id still parses"), WithoutVersion.Id, TEXT("realm-a"));
  TestEqual(TEXT("name still parses"), WithoutVersion.Name, TEXT("Realm A"));
  TestTrue(TEXT("listed still parses"), WithoutVersion.bListed);

  const FRedwoodRealm NullVersion =
    ParseRealmJson(TEXT(", \"version\": null"));
  TestEqual(TEXT("null version is empty"), NullVersion.Version, FString());

  return true;
}
