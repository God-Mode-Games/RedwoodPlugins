// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins URedwoodSettings::ReadSignedRealm, which lets the signed redwood.json
// pin the client to one realm:
//   1. A validly signed file yields its realmId and realmUri.
//   2. A file whose realm fields were changed after signing yields nothing,
//      so a tampered install cannot redirect the client to another realm.
//   3. An unsigned file yields nothing once a signing key is configured.
//   4. With no signing key, the file is trusted as-is, like directorUri.
//   5. A missing file or missing fields yield empty strings.
// The key pair and signature below were made offline for this test only;
// the private key was discarded.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RedwoodSettings.h"

namespace {
  const TCHAR *const TestPublicKey =
    TEXT("Gdn+iT3hSs6sSoQ4fiOozjyxji6UmCE+LcFUuO97Zeo=");

  // Signs the canonical form of TestPayloadFields below.
  const TCHAR *const TestSignature = TEXT(
    "965CL+06I59HsxT0e05ufNLEL0wAajCjaC1NhEbkOtYjwAdHgpwA4K22UW5vbTBNBe5WMHxKum2Uu/ZL/fDICQ=="
  );

  const TCHAR *const TestPayloadFields = TEXT(
    "\"directorUri\": \"wss://director.example.test\", "
    "\"realmId\": \"realm-test-id\", "
    "\"realmUri\": \"wss://realm.example.test\""
  );

  FString WriteConfig(const FString &FileName, const FString &Json) {
    const FString Path =
      FPaths::AutomationTransientDir() / TEXT("SignedRealm") / FileName;
    FFileHelper::SaveStringToFile(Json, *Path);
    return Path;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSignedRealmTest,
  "Redwood.Settings.SignedRealm",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSignedRealmTest::RunTest(const FString &Parameters) {
  FString RealmId;
  FString RealmUri;

  const FString SignedPath = WriteConfig(
    TEXT("signed.json"),
    FString::Printf(
      TEXT("{%s, \"signature\": \"%s\"}"), TestPayloadFields, TestSignature
    )
  );
  URedwoodSettings::ReadSignedRealm(
    SignedPath, TestPublicKey, RealmId, RealmUri
  );
  TestEqual(TEXT("signed realmId is read"), RealmId, FString(TEXT("realm-test-id")));
  TestEqual(
    TEXT("signed realmUri is read"),
    RealmUri,
    FString(TEXT("wss://realm.example.test"))
  );

  const FString TamperedPath = WriteConfig(
    TEXT("tampered.json"),
    FString::Printf(
      TEXT(
        "{\"directorUri\": \"wss://director.example.test\", "
        "\"realmId\": \"realm-test-id\", "
        "\"realmUri\": \"wss://other.example.test\", \"signature\": \"%s\"}"
      ),
      TestSignature
    )
  );
  AddExpectedErrorPlain(TEXT("failed Ed25519 verification"));
  URedwoodSettings::ReadSignedRealm(
    TamperedPath, TestPublicKey, RealmId, RealmUri
  );
  TestTrue(
    TEXT("a realm changed after signing is refused"),
    RealmId.IsEmpty() && RealmUri.IsEmpty()
  );

  const FString UnsignedPath = WriteConfig(
    TEXT("unsigned.json"), FString::Printf(TEXT("{%s}"), TestPayloadFields)
  );
  AddExpectedErrorPlain(TEXT("has no `signature` field"));
  URedwoodSettings::ReadSignedRealm(
    UnsignedPath, TestPublicKey, RealmId, RealmUri
  );
  TestTrue(
    TEXT("an unsigned file is refused when a key is configured"),
    RealmId.IsEmpty() && RealmUri.IsEmpty()
  );

  URedwoodSettings::ReadSignedRealm(
    UnsignedPath, FString(), RealmId, RealmUri
  );
  TestEqual(
    TEXT("with no key the file is trusted, like directorUri"),
    RealmId,
    FString(TEXT("realm-test-id"))
  );

  const FString NoRealmPath = WriteConfig(
    TEXT("no-realm.json"),
    TEXT("{\"directorUri\": \"wss://director.example.test\"}")
  );
  URedwoodSettings::ReadSignedRealm(NoRealmPath, FString(), RealmId, RealmUri);
  TestTrue(
    TEXT("a file without realm fields leaves both empty"),
    RealmId.IsEmpty() && RealmUri.IsEmpty()
  );

  RealmId = TEXT("stale");
  URedwoodSettings::ReadSignedRealm(
    FPaths::AutomationTransientDir() / TEXT("SignedRealm") / TEXT("missing.json"),
    TestPublicKey,
    RealmId,
    RealmUri
  );
  TestTrue(TEXT("a missing file clears stale output"), RealmId.IsEmpty());

  IFileManager::Get().DeleteDirectory(
    *(FPaths::AutomationTransientDir() / TEXT("SignedRealm")), false, true
  );
  return true;
}
