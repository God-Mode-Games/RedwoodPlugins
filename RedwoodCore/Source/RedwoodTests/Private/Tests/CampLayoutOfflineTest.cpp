// Copyright Incanta Games. All Rights Reserved.

// FORK(hollowed-oath): covers the offline (no backend) path of the camp record
// calls. Callers must get the same output shape as from the backend, and the
// player id becomes a file name so it must be validated.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
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

  FString PutCamp(
    URedwoodServerGameSubsystem *Subsystem,
    const FString &PlayerId,
    USIOJsonObject *Record,
    USIOJsonObject *Layout
  ) {
    FString Result = TEXT("delegate not called");
    Subsystem->PutCampLayout(
      PlayerId,
      3,
      Record,
      Layout,
      FRedwoodErrorOutputDelegate::CreateLambda([&Result](const FString &Error) {
        Result = Error;
      })
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

  TestEqual(
    TEXT("Put succeeds"),
    PutCamp(Subsystem, CampTestPlayerId, Record, Layout),
    FString()
  );

  FRedwoodGetCampLayoutOutput Found = GetCamp(Subsystem, CampTestPlayerId);
  TestTrue(TEXT("Stored camp has no error"), Found.Error.IsEmpty());
  TestTrue(TEXT("Stored camp is found"), Found.Camp.bFound);
  TestEqual(TEXT("Schema version round-trips"), Found.Camp.SchemaVersion, 3);
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

  // Path separators and dots must never reach the file name.
  const FString BadId = TEXT("..\\evil");
  TestFalse(
    TEXT("Put rejects a bad id"),
    PutCamp(Subsystem, BadId, Record, Layout).IsEmpty()
  );
  TestFalse(
    TEXT("Get rejects a bad id"), GetCamp(Subsystem, BadId).Error.IsEmpty()
  );

  IFileManager::Get().Delete(*CampTestFile());
  return true;
}
