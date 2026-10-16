// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

// Covers the account document calls: the offline (no backend) store, which
// must give the same output as the backend, and the parsing of backend
// replies. The player id and the key become file names, so they must be
// validated.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "RedwoodAccountDocuments.h"
#include "RedwoodServerGameSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace {
  const FString TestPlayerId = TEXT("redwood-test-account-documents");
  const FString TestKey = TEXT("camp");

  struct FGetCall {
    bool bCalled = false;
    FRedwoodGetAccountDocumentOutput Output;
  };

  struct FPutCall {
    bool bCalled = false;
    FRedwoodPutAccountDocumentOutput Output;
  };

  // The offline path answers before the call returns, so the result is ready
  // here. bCalled stays false if the delegate never runs, and the caller fails.
  FGetCall Get(
    URedwoodServerGameSubsystem *Subsystem,
    const FString &PlayerId,
    const FString &Key = TestKey
  ) {
    FGetCall Call;
    Subsystem->GetAccountDocument(
      PlayerId,
      Key,
      FRedwoodGetAccountDocumentOutputDelegate::CreateLambda(
        [&Call](const FRedwoodGetAccountDocumentOutput &Output) {
          Call.bCalled = true;
          Call.Output = Output;
        }
      )
    );
    return Call;
  }

  FPutCall Put(
    URedwoodServerGameSubsystem *Subsystem,
    const FString &PlayerId,
    const TSharedPtr<FJsonObject> &Data,
    int32 ExpectedRevision,
    const FString &Key = TestKey
  ) {
    FPutCall Call;
    Subsystem->PutAccountDocument(
      PlayerId,
      Key,
      Data,
      ExpectedRevision,
      FRedwoodPutAccountDocumentOutputDelegate::CreateLambda(
        [&Call](const FRedwoodPutAccountDocumentOutput &Output) {
          Call.bCalled = true;
          Call.Output = Output;
        }
      )
    );
    return Call;
  }

  TSharedPtr<FJsonObject> MakeData(int32 Level) {
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("schemaVersion"), 3);
    Data->SetNumberField(TEXT("level"), Level);
    return Data;
  }

  int32 GetLevel(const FGetCall &Call) {
    const TSharedPtr<FJsonObject> &Data = Call.Output.Document.Data;
    return Data.IsValid() ? static_cast<int32>(Data->GetNumberField(TEXT("level")))
                          : -1;
  }

  // Objects and arrays nested Depth levels deep, the returned object being
  // level 1, as the backend counts them.
  TSharedPtr<FJsonObject> MakeNested(int32 Depth) {
    TSharedPtr<FJsonObject> Leaf = MakeShared<FJsonObject>();
    Leaf->SetBoolField(TEXT("leaf"), true);
    TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Leaf);
    for (int32 Level = 2; Level < Depth; ++Level) {
      if (Level % 2 == 0) {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetField(TEXT("child"), Value);
        Value = MakeShared<FJsonValueObject>(Wrapper);
      } else {
        Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Value});
      }
    }
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetField(TEXT("child"), Value);
    return Root;
  }

  // A reply as the socket gives it: one JSON value.
  TArray<TSharedPtr<FJsonValue>> MakeReply(const FString &Json) {
    TSharedPtr<FJsonValue> Value;
    TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
    FJsonSerializer::Deserialize(Reader, Value);
    TArray<TSharedPtr<FJsonValue>> Reply;
    if (Value.IsValid()) {
      Reply.Add(Value);
    }
    return Reply;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodAccountDocumentsOfflineTest,
  "Redwood.AccountDocuments.Offline",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodAccountDocumentsOfflineTest::RunTest(const FString &Parameters) {
  const FString MainFile =
    RedwoodAccountDocuments::GetOfflinePath(TestPlayerId, TestKey);
  const FString TempFile = MainFile + TEXT(".tmp");
  IFileManager &FileManager = IFileManager::Get();
  FileManager.Delete(*MainFile);
  FileManager.Delete(*TempFile);

  // A subsystem needs a game instance as outer. With no world, ShouldUseBackend
  // is false, which selects the offline path.
  UGameInstance *GameInstance = NewObject<UGameInstance>(GEngine);
  URedwoodServerGameSubsystem *Subsystem =
    NewObject<URedwoodServerGameSubsystem>(GameInstance);

  const FGetCall Missing = Get(Subsystem, TestPlayerId);
  TestTrue(TEXT("Get calls back"), Missing.bCalled);
  TestTrue(TEXT("A missing document has no error"), Missing.Output.Error.IsEmpty());
  TestFalse(TEXT("A missing document is not found"), Missing.Output.bFound);

  const FPutCall Created = Put(Subsystem, TestPlayerId, MakeData(7), 0);
  TestTrue(TEXT("Put calls back"), Created.bCalled);
  TestTrue(TEXT("Create succeeds"), Created.Output.Error.IsEmpty());
  TestFalse(TEXT("Create is not a conflict"), Created.Output.bConflict);
  TestEqual(TEXT("Create gives revision 1"), Created.Output.Revision, 1);
  TestFalse(TEXT("The write leaves no temp file"), FileManager.FileExists(*TempFile));

  const FGetCall Found = Get(Subsystem, TestPlayerId);
  TestTrue(TEXT("Read back calls back"), Found.bCalled);
  TestTrue(TEXT("Read back has no error"), Found.Output.Error.IsEmpty());
  TestTrue(TEXT("Read back is found"), Found.Output.bFound);
  TestEqual(TEXT("Revision round-trips"), Found.Output.Document.Revision, 1);
  TestEqual(TEXT("Data round-trips"), GetLevel(Found), 7);

  // Another key of the same account is another document.
  const FGetCall OtherKey = Get(Subsystem, TestPlayerId, TEXT("flags"));
  TestTrue(TEXT("Other key calls back"), OtherKey.bCalled);
  TestFalse(TEXT("Another key is a separate document"), OtherKey.Output.bFound);

  const FPutCall SecondCreate = Put(Subsystem, TestPlayerId, MakeData(8), 0);
  TestTrue(TEXT("Second create calls back"), SecondCreate.bCalled);
  TestTrue(TEXT("Create over an existing document conflicts"), SecondCreate.Output.bConflict);
  TestTrue(TEXT("A conflict is not an error"), SecondCreate.Output.Error.IsEmpty());

  const FPutCall Updated = Put(Subsystem, TestPlayerId, MakeData(99), 1);
  TestTrue(TEXT("Update calls back"), Updated.bCalled);
  TestTrue(TEXT("Update succeeds"), Updated.Output.Error.IsEmpty());
  TestFalse(TEXT("Update at the right revision is no conflict"), Updated.Output.bConflict);
  TestEqual(TEXT("Update bumps the revision"), Updated.Output.Revision, 2);

  const FPutCall Stale = Put(Subsystem, TestPlayerId, MakeData(1), 1);
  TestTrue(TEXT("Stale put calls back"), Stale.bCalled);
  TestTrue(TEXT("A stale update conflicts"), Stale.Output.bConflict);
  const FGetCall AfterStale = Get(Subsystem, TestPlayerId);
  TestEqual(TEXT("A stale update leaves the revision"), AfterStale.Output.Document.Revision, 2);
  TestEqual(TEXT("A stale update leaves the data"), GetLevel(AfterStale), 99);

  // A crash in the move can leave only the temp file, which holds the whole
  // new document.
  FileManager.Move(*TempFile, *MainFile, true);
  TestFalse(TEXT("The crash leaves no main file"), FileManager.FileExists(*MainFile));
  const FGetCall Recovered = Get(Subsystem, TestPlayerId);
  TestTrue(TEXT("Recovery calls back"), Recovered.bCalled);
  TestTrue(TEXT("The document is read from the temp file"), Recovered.Output.bFound);
  TestEqual(TEXT("with its revision"), Recovered.Output.Document.Revision, 2);
  TestTrue(
    TEXT("and moved back in place"),
    FileManager.FileExists(*MainFile) && !FileManager.FileExists(*TempFile)
  );

  // A torn temp file with no main file is a put that never finished.
  FileManager.Delete(*MainFile);
  FFileHelper::SaveStringToFile(TEXT("{ \"revision\": 3, \"da"), *TempFile);
  const FGetCall Torn = Get(Subsystem, TestPlayerId);
  TestTrue(TEXT("Torn read calls back"), Torn.bCalled);
  TestTrue(TEXT("A torn temp file is no error"), Torn.Output.Error.IsEmpty());
  TestFalse(TEXT("A torn temp file is no document"), Torn.Output.bFound);
  TestFalse(TEXT("The torn temp file is deleted"), FileManager.FileExists(*TempFile));

  // A main file that exists but is garbage is an error, and a put must not
  // overwrite it.
  const FString CorruptContents = TEXT("{ not json");
  FFileHelper::SaveStringToFile(CorruptContents, *MainFile);
  const FGetCall Corrupt = Get(Subsystem, TestPlayerId);
  TestTrue(TEXT("Corrupt read calls back"), Corrupt.bCalled);
  TestFalse(TEXT("A corrupt main file is an error"), Corrupt.Output.Error.IsEmpty());
  TestFalse(TEXT("A corrupt main file is not found"), Corrupt.Output.bFound);
  const FPutCall OverCorrupt = Put(Subsystem, TestPlayerId, MakeData(5), 0);
  TestTrue(TEXT("Put over corrupt calls back"), OverCorrupt.bCalled);
  TestFalse(TEXT("A put over a corrupt main file is an error"), OverCorrupt.Output.Error.IsEmpty());
  FString AfterRefusal;
  FFileHelper::LoadFileToString(AfterRefusal, *MainFile);
  TestEqual(TEXT("The refused put leaves the corrupt file as it was"), AfterRefusal, CorruptContents);

  // Path separators, dots, non-ASCII letters and long ids must never reach a
  // file name, and the backend refuses them too.
  const TArray<FString> BadIds = {
    TEXT(""), TEXT("..\\evil"), TEXT("a/b"), TEXT("a.b"), TEXT("café"),
    FString::ChrN(RedwoodAccountDocumentLimits::MaxIdLength + 1, TEXT('a')),
  };
  for (const FString &BadId : BadIds) {
    const FPutCall BadPlayerPut = Put(Subsystem, BadId, MakeData(1), 0);
    TestTrue(TEXT("Bad id put calls back"), BadPlayerPut.bCalled);
    TestFalse(FString::Printf(TEXT("Put rejects player id '%s'"), *BadId), BadPlayerPut.Output.Error.IsEmpty());
    const FGetCall BadPlayerGet = Get(Subsystem, BadId);
    TestTrue(TEXT("Bad id get calls back"), BadPlayerGet.bCalled);
    TestFalse(FString::Printf(TEXT("Get rejects player id '%s'"), *BadId), BadPlayerGet.Output.Error.IsEmpty());
    const FGetCall BadKeyGet = Get(Subsystem, TestPlayerId, BadId);
    TestTrue(TEXT("Bad key get calls back"), BadKeyGet.bCalled);
    TestFalse(FString::Printf(TEXT("Get rejects key '%s'"), *BadId), BadKeyGet.Output.Error.IsEmpty());
  }
  // Keys are lowercase and not a device name, so no two keys can be one
  // file on Windows.
  const TArray<FString> BadKeys = {
    TEXT("Camp"), TEXT("CAMP"), TEXT("con"), TEXT("nul"), TEXT("com1"), TEXT("lpt9"),
  };
  for (const FString &BadKey : BadKeys) {
    const FPutCall BadKeyPut = Put(Subsystem, TestPlayerId, MakeData(1), 0, BadKey);
    TestTrue(TEXT("Bad key put calls back"), BadKeyPut.bCalled);
    TestFalse(FString::Printf(TEXT("Put rejects key '%s'"), *BadKey), BadKeyPut.Output.Error.IsEmpty());
    const FGetCall BadKeyGet = Get(Subsystem, TestPlayerId, BadKey);
    TestTrue(TEXT("Bad key get calls back"), BadKeyGet.bCalled);
    TestFalse(FString::Printf(TEXT("Get rejects key '%s'"), *BadKey), BadKeyGet.Output.Error.IsEmpty());
  }
  TestTrue(TEXT("A key that only starts with a device name is valid"), RedwoodAccountDocuments::IsValidKey(TEXT("con1")));

  TestTrue(
    TEXT("An id of the maximum length is valid"),
    RedwoodAccountDocuments::IsValidId(
      FString::ChrN(RedwoodAccountDocumentLimits::MaxIdLength, TEXT('a'))
    )
  );

  FileManager.Delete(*MainFile);

  const FPutCall NoData = Put(Subsystem, TestPlayerId, nullptr, 0);
  TestTrue(TEXT("No data put calls back"), NoData.bCalled);
  TestFalse(TEXT("A put needs data"), NoData.Output.Error.IsEmpty());

  // Too large is refused before anything is written or sent.
  TSharedPtr<FJsonObject> Large = MakeShared<FJsonObject>();
  Large->SetStringField(
    TEXT("blob"), FString::ChrN(RedwoodAccountDocumentLimits::MaxDataBytes, TEXT('x'))
  );
  const FPutCall TooLarge = Put(Subsystem, TestPlayerId, Large, 0);
  TestTrue(TEXT("Too large put calls back"), TooLarge.bCalled);
  TestTrue(TEXT("A document above the limit is too large"), TooLarge.Output.bTooLarge);
  TestTrue(TEXT("Too large is final"), TooLarge.Output.bRejected);
  TestFalse(TEXT("Too large is not a conflict"), TooLarge.Output.bConflict);
  TestFalse(TEXT("Too large writes nothing"), FileManager.FileExists(*MainFile));

  // Too deep is refused before anything is written or sent, the same as the
  // backend does it.
  const FPutCall TooDeep =
    Put(Subsystem, TestPlayerId, MakeNested(RedwoodAccountDocumentLimits::MaxDepth + 1), 0);
  TestTrue(TEXT("Too deep put calls back"), TooDeep.bCalled);
  TestTrue(TEXT("A document above the depth limit is final"), TooDeep.Output.bRejected);
  TestFalse(TEXT("Too deep is not too large"), TooDeep.Output.bTooLarge);
  TestFalse(TEXT("Too deep has an error"), TooDeep.Output.Error.IsEmpty());
  TestFalse(TEXT("Too deep writes nothing"), FileManager.FileExists(*MainFile));

  const FPutCall AtDepthLimit =
    Put(Subsystem, TestPlayerId, MakeNested(RedwoodAccountDocumentLimits::MaxDepth), 0);
  TestTrue(TEXT("Depth limit put calls back"), AtDepthLimit.bCalled);
  TestTrue(TEXT("A document at the depth limit is stored"), AtDepthLimit.Output.Error.IsEmpty());
  TestFalse(TEXT("A document at the depth limit is not final"), AtDepthLimit.Output.bRejected);

  FileManager.Delete(*MainFile);
  FileManager.Delete(*TempFile);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodAccountDocumentsReplyTest,
  "Redwood.AccountDocuments.ParseReplies",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodAccountDocumentsReplyTest::RunTest(const FString &Parameters) {
  using namespace RedwoodAccountDocuments;

  const FRedwoodGetAccountDocumentOutput Found = ParseGetReply(
    MakeReply(TEXT("{\"error\":\"\",\"document\":{\"revision\":4,\"data\":{\"level\":2}}}"))
  );
  TestTrue(TEXT("A good get reply has no error"), Found.Error.IsEmpty());
  TestTrue(TEXT("A good get reply is found"), Found.bFound);
  TestEqual(TEXT("with its revision"), Found.Document.Revision, 4);
  TestTrue(TEXT("and its data"), Found.Document.Data.IsValid());

  const FRedwoodGetAccountDocumentOutput Null =
    ParseGetReply(MakeReply(TEXT("{\"error\":\"\",\"document\":null}")));
  TestTrue(TEXT("A null document is no error"), Null.Error.IsEmpty());
  TestFalse(TEXT("A null document is not found"), Null.bFound);

  const FRedwoodGetAccountDocumentOutput BackendError =
    ParseGetReply(MakeReply(TEXT("{\"error\":\"nope\",\"document\":null}")));
  TestEqual(TEXT("A backend error is passed on"), BackendError.Error, FString(TEXT("nope")));

  const TArray<FString> BadGetReplies = {
    TEXT("[]"),
    TEXT("\"text\""),
    TEXT("{\"document\":null}"),
    TEXT("{\"error\":1,\"document\":null}"),
    TEXT("{\"error\":\"\"}"),
    TEXT("{\"error\":\"\",\"document\":\"x\"}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":1}}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":\"1\",\"data\":{}}}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":1.5,\"data\":{}}}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":-1,\"data\":{}}}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":3000000000,\"data\":{}}}"),
    TEXT("{\"error\":\"\",\"document\":{\"revision\":1,\"data\":[]}}"),
  };
  for (const FString &Reply : BadGetReplies) {
    const FRedwoodGetAccountDocumentOutput Output = ParseGetReply(MakeReply(Reply));
    TestFalse(FString::Printf(TEXT("Get reply %s is an error"), *Reply), Output.Error.IsEmpty());
    TestFalse(FString::Printf(TEXT("Get reply %s is not found"), *Reply), Output.bFound);
  }
  TestFalse(TEXT("An empty get reply is an error"), ParseGetReply({}).Error.IsEmpty());

  const FRedwoodPutAccountDocumentOutput Stored = ParsePutReply(
    MakeReply(TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":5,\"tooLarge\":false,\"rejected\":false}"))
  );
  TestTrue(TEXT("A good put reply has no error"), Stored.Error.IsEmpty());
  TestEqual(TEXT("A good put reply has the revision"), Stored.Revision, 5);

  const FRedwoodPutAccountDocumentOutput Conflict = ParsePutReply(
    MakeReply(TEXT("{\"error\":\"\",\"conflict\":true,\"revision\":0,\"tooLarge\":false,\"rejected\":false}"))
  );
  TestTrue(TEXT("A conflict reply is a conflict"), Conflict.bConflict);
  TestTrue(TEXT("A conflict reply is no error"), Conflict.Error.IsEmpty());

  const FRedwoodPutAccountDocumentOutput TooLarge = ParsePutReply(MakeReply(
    TEXT("{\"error\":\"too big\",\"conflict\":false,\"revision\":0,\"tooLarge\":true,\"rejected\":true}")
  ));
  TestTrue(TEXT("A too large reply sets bTooLarge"), TooLarge.bTooLarge);
  TestFalse(TEXT("A too large reply keeps its error"), TooLarge.Error.IsEmpty());
  TestTrue(TEXT("A too large reply is final"), TooLarge.bRejected);

  const FRedwoodPutAccountDocumentOutput Rejected = ParsePutReply(MakeReply(
    TEXT("{\"error\":\"too deep\",\"conflict\":false,\"revision\":0,\"tooLarge\":false,\"rejected\":true}")
  ));
  TestTrue(TEXT("A rejected reply sets bRejected"), Rejected.bRejected);
  TestFalse(TEXT("A rejected reply is not too large"), Rejected.bTooLarge);
  TestFalse(TEXT("A rejected reply keeps its error"), Rejected.Error.IsEmpty());

  const TArray<FString> BadPutReplies = {
    TEXT("[]"),
    TEXT("{\"conflict\":false,\"revision\":1,\"tooLarge\":false,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"revision\":1,\"tooLarge\":false,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"tooLarge\":false,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":1,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":1,\"tooLarge\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":1,\"tooLarge\":false,\"rejected\":\"false\"}"),
    TEXT("{\"error\":\"\",\"conflict\":\"true\",\"revision\":1,\"tooLarge\":false,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":\"1\",\"tooLarge\":false,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":1,\"tooLarge\":1,\"rejected\":false}"),
    TEXT("{\"error\":\"\",\"conflict\":false,\"revision\":1e12,\"tooLarge\":false,\"rejected\":false}"),
  };
  for (const FString &Reply : BadPutReplies) {
    const FRedwoodPutAccountDocumentOutput Output = ParsePutReply(MakeReply(Reply));
    TestFalse(FString::Printf(TEXT("Put reply %s is an error"), *Reply), Output.Error.IsEmpty());
    TestFalse(FString::Printf(TEXT("Put reply %s is not a conflict"), *Reply), Output.bConflict);
    TestEqual(FString::Printf(TEXT("Put reply %s has no revision"), *Reply), Output.Revision, 0);
  }
  return true;
}
