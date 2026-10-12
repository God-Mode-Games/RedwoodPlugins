// Copyright Incanta Games. All Rights Reserved.

// FORK(hollowed-oath): covers the offline (no backend) path of the camp record
// calls. Callers must get the same output shape as from the backend, and the
// player id becomes a file name so it must be validated.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RedwoodServerGameSubsystem.h"
#include "SIOJsonObject.h"
#include "Types/RedwoodTypesCamps.h"

namespace {
  const FString CampTestPlayerId = TEXT("redwood-test-camp-player");

  FString CampTestFile() {
    return FPaths::ProjectSavedDir() / TEXT("Persistence") / TEXT("Camps") /
      (CampTestPlayerId + TEXT(".json"));
  }

  FRedwoodGetCampLayoutOutput GetCamp(
    URedwoodServerGameSubsystem *Subsystem, const FString &PlayerId
  ) {
    FRedwoodGetCampLayoutOutput Result;
    Subsystem->GetCampLayout(
      PlayerId,
      FRedwoodGetCampLayoutOutputDelegate::CreateLambda(
        [&Result](const FRedwoodGetCampLayoutOutput &Output) {
          Result = Output;
        }
      )
    );
    return Result;
  }

  FRedwoodPutCampLayoutOutput PutCamp(
    URedwoodServerGameSubsystem *Subsystem,
    const FString &PlayerId,
    USIOJsonObject *Record,
    USIOJsonObject *Layout,
    int32 ExpectedRevision
  ) {
    FRedwoodPutCampLayoutOutput Result;
    Result.Error = TEXT("delegate not called");
    Subsystem->PutCampLayout(
      PlayerId,
      3,
      Record,
      Layout,
      ExpectedRevision,
      FRedwoodPutCampLayoutOutputDelegate::CreateLambda(
        [&Result](const FRedwoodPutCampLayoutOutput &Output) {
          Result = Output;
        }
      )
    );
    return Result;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCampLayoutOfflineTest,
  "Redwood.Camps.OfflineRoundTrip",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCampLayoutOfflineTest::RunTest(const FString &Parameters) {
  IFileManager::Get().Delete(*CampTestFile());

  // A subsystem needs a game instance as outer. With no world, ShouldUseBackend
  // is false, which selects the offline path.
  UGameInstance *GameInstance = NewObject<UGameInstance>(GEngine);
  URedwoodServerGameSubsystem *Subsystem =
    NewObject<URedwoodServerGameSubsystem>(GameInstance);

  FRedwoodGetCampLayoutOutput Missing = GetCamp(Subsystem, CampTestPlayerId);
  TestTrue(TEXT("Missing camp has no error"), Missing.Error.IsEmpty());
  TestFalse(TEXT("Missing camp is not found"), Missing.Camp.bFound);

  USIOJsonObject *Record = NewObject<USIOJsonObject>();
  Record->SetNumberField(TEXT("level"), 7);
  USIOJsonObject *Layout = NewObject<USIOJsonObject>();
  Layout->SetStringField(TEXT("tent"), TEXT("north"));

  const FRedwoodPutCampLayoutOutput Created =
    PutCamp(Subsystem, CampTestPlayerId, Record, Layout, 0);
  TestTrue(TEXT("Create succeeds"), Created.Error.IsEmpty());
  TestFalse(TEXT("Create is not a conflict"), Created.bConflict);
  TestEqual(TEXT("Create gives revision 1"), Created.Revision, 1);

  FRedwoodGetCampLayoutOutput Found = GetCamp(Subsystem, CampTestPlayerId);
  TestTrue(TEXT("Stored camp has no error"), Found.Error.IsEmpty());
  TestTrue(TEXT("Stored camp is found"), Found.Camp.bFound);
  TestEqual(TEXT("Schema version round-trips"), Found.Camp.SchemaVersion, 3);
  TestEqual(TEXT("Revision round-trips"), Found.Camp.Revision, 1);
  if (TestNotNull(TEXT("Record round-trips"), Found.Camp.Record)) {
    TestEqual(
      TEXT("Record field"),
      static_cast<int32>(Found.Camp.Record->GetNumberField(TEXT("level"))),
      7
    );
  }
  if (TestNotNull(TEXT("Layout round-trips"), Found.Camp.Layout)) {
    TestEqual(
      TEXT("Layout field"),
      Found.Camp.Layout->GetStringField(TEXT("tent")),
      FString(TEXT("north"))
    );
  }

  // A second create must lose: the row exists.
  const FRedwoodPutCampLayoutOutput SecondCreate =
    PutCamp(Subsystem, CampTestPlayerId, Record, Layout, 0);
  TestTrue(TEXT("Create over an existing row conflicts"), SecondCreate.bConflict);

  // A stale revision must lose and leave the file as it was.
  USIOJsonObject *OtherRecord = NewObject<USIOJsonObject>();
  OtherRecord->SetNumberField(TEXT("level"), 99);
  const FRedwoodPutCampLayoutOutput Updated =
    PutCamp(Subsystem, CampTestPlayerId, OtherRecord, Layout, 1);
  TestFalse(TEXT("Update at the right revision is no conflict"), Updated.bConflict);
  TestEqual(TEXT("Update bumps the revision"), Updated.Revision, 2);

  const FRedwoodPutCampLayoutOutput Stale =
    PutCamp(Subsystem, CampTestPlayerId, Record, Layout, 1);
  TestTrue(TEXT("Stale update conflicts"), Stale.bConflict);
  TestTrue(TEXT("A conflict is not an error"), Stale.Error.IsEmpty());
  FRedwoodGetCampLayoutOutput AfterStale = GetCamp(Subsystem, CampTestPlayerId);
  TestEqual(TEXT("Stale update left the revision"), AfterStale.Camp.Revision, 2);
  if (TestNotNull(TEXT("Record still there"), AfterStale.Camp.Record)) {
    TestEqual(
      TEXT("Stale update left the record"),
      static_cast<int32>(AfterStale.Camp.Record->GetNumberField(TEXT("level"))),
      99
    );
  }

  // A file that exists but is garbage is an error, not "no camp", and a put must
  // not overwrite it.
  FFileHelper::SaveStringToFile(TEXT("{ not json"), *CampTestFile());
  FRedwoodGetCampLayoutOutput Corrupt = GetCamp(Subsystem, CampTestPlayerId);
  TestFalse(TEXT("Corrupt file is an error"), Corrupt.Error.IsEmpty());
  TestFalse(TEXT("Corrupt file is not a camp"), Corrupt.Camp.bFound);
  TestFalse(
    TEXT("Put over a corrupt file is an error"),
    PutCamp(Subsystem, CampTestPlayerId, Record, Layout, 0).Error.IsEmpty()
  );

  // Path separators and dots must never reach the file name.
  const FString BadId = TEXT("..\\evil");
  TestFalse(
    TEXT("Put rejects a bad id"),
    PutCamp(Subsystem, BadId, Record, Layout, 0).Error.IsEmpty()
  );
  TestFalse(
    TEXT("Get rejects a bad id"), GetCamp(Subsystem, BadId).Error.IsEmpty()
  );

  IFileManager::Get().Delete(*CampTestFile());
  return true;
}
