// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodAccountDocuments.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace {
  const TCHAR *MalformedReplyError = TEXT("Malformed account document reply");

  // The Try* getters of FJsonObject convert between types (a string "7" reads
  // as a number, a number reads as a string). A reply of the wrong shape must
  // be an error, so these accept only the exact JSON type.
  bool TryGetTypedField(
    const TSharedPtr<FJsonObject> &Object,
    const TCHAR *Field,
    EJson Type,
    TSharedPtr<FJsonValue> &OutValue
  ) {
    OutValue = Object->TryGetField(Field);
    return OutValue.IsValid() && OutValue->Type == Type;
  }

  bool TryGetStrictString(
    const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, FString &OutValue
  ) {
    TSharedPtr<FJsonValue> Value;
    if (!TryGetTypedField(Object, Field, EJson::String, Value)) {
      return false;
    }
    OutValue = Value->AsString();
    return true;
  }

  bool TryGetStrictBool(
    const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, bool &OutValue
  ) {
    TSharedPtr<FJsonValue> Value;
    if (!TryGetTypedField(Object, Field, EJson::Boolean, Value)) {
      return false;
    }
    OutValue = Value->AsBool();
    return true;
  }

  // A revision is a whole number in 0..MAX_int32. A cast of any other double
  // to int32 is undefined or wrong, so it is checked first.
  bool TryGetRevision(
    const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, int32 &OutValue
  ) {
    TSharedPtr<FJsonValue> Value;
    if (!TryGetTypedField(Object, Field, EJson::Number, Value)) {
      return false;
    }
    const double Number = Value->AsNumber();
    // Written so that NaN fails too.
    if (!(Number >= 0.0 && Number <= static_cast<double>(MAX_int32)) ||
        FMath::FloorToDouble(Number) != Number) {
      return false;
    }
    OutValue = static_cast<int32>(Number);
    return true;
  }

  // The backend's `document` and the offline file have the same shape:
  // { revision, data }.
  bool TryParseDocument(
    const TSharedPtr<FJsonObject> &Object, FRedwoodAccountDocument &OutDocument
  ) {
    TSharedPtr<FJsonValue> DataValue;
    int32 Revision = 0;
    if (!Object.IsValid() || !TryGetRevision(Object, TEXT("revision"), Revision) ||
        !TryGetTypedField(Object, TEXT("data"), EJson::Object, DataValue)) {
      return false;
    }
    OutDocument.Revision = Revision;
    OutDocument.Data = DataValue->AsObject();
    return true;
  }

  TSharedPtr<FJsonObject> GetReplyObject(
    const TArray<TSharedPtr<FJsonValue>> &Response
  ) {
    if (Response.Num() == 0 || !Response[0].IsValid() ||
        Response[0]->Type != EJson::Object) {
      return nullptr;
    }
    return Response[0]->AsObject();
  }

  FString GetOfflineTempPath(const FString &PlayerId, const FString &Key) {
    return RedwoodAccountDocuments::GetOfflinePath(PlayerId, Key) + TEXT(".tmp");
  }

  enum class EReadFileResult { Read, Unreadable, Unparseable };

  EReadFileResult ReadDocumentFile(
    const FString &FileName, FRedwoodAccountDocument &OutDocument
  ) {
    FString FileContents;
    if (!FFileHelper::LoadFileToString(FileContents, *FileName)) {
      return EReadFileResult::Unreadable;
    }

    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<TCHAR>> JsonReader =
      TJsonReaderFactory<TCHAR>::Create(FileContents);
    if (!FJsonSerializer::Deserialize(JsonReader, Object) ||
        !TryParseDocument(Object, OutDocument)) {
      return EReadFileResult::Unparseable;
    }
    return EReadFileResult::Read;
  }

  // bDoNotRetryOrError: the default move retries for seconds on a failure,
  // which would stall the game thread. A failed move is handled by the caller.
  bool MoveOverTarget(const FString &FileName, const FString &TempFileName) {
    return IFileManager::Get().Move(
      *FileName,
      *TempFileName,
      /*Replace*/ true,
      /*EvenIfReadOnly*/ false,
      /*Attributes*/ false,
      /*bDoNotRetryOrError*/ true
    );
  }
} // namespace

namespace RedwoodAccountDocuments {
  bool IsValidId(const FString &Id) {
    if (Id.IsEmpty() || Id.Len() > RedwoodAccountDocumentLimits::MaxIdLength) {
      return false;
    }
    // ASCII only: FChar::IsAlnum also accepts letters of other alphabets,
    // which the backend refuses.
    for (const TCHAR Character : Id) {
      const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z')) ||
        (Character >= TEXT('A') && Character <= TEXT('Z')) ||
        (Character >= TEXT('0') && Character <= TEXT('9')) ||
        Character == TEXT('_') || Character == TEXT('-');
      if (!bAllowed) {
        return false;
      }
    }
    return true;
  }

  bool IsTooLarge(const TSharedRef<FJsonObject> &Data) {
    // The backend measures JSON.stringify(data) in UTF-8 bytes. This text can
    // differ a little (number format), so the backend check stays the rule;
    // this one only stops a send that is sure to be refused.
    FString Text;
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Data, Writer);
    return FTCHARToUTF8(*Text).Length() >
      RedwoodAccountDocumentLimits::MaxDataBytes;
  }

  FRedwoodGetAccountDocumentOutput ParseGetReply(
    const TArray<TSharedPtr<FJsonValue>> &Response
  ) {
    FRedwoodGetAccountDocumentOutput Result;
    const TSharedPtr<FJsonObject> Reply = GetReplyObject(Response);
    if (!Reply.IsValid() || !TryGetStrictString(Reply, TEXT("error"), Result.Error)) {
      Result.Error = MalformedReplyError;
      return Result;
    }
    if (!Result.Error.IsEmpty()) {
      return Result;
    }

    const TSharedPtr<FJsonValue> DocumentValue = Reply->TryGetField(TEXT("document"));
    if (DocumentValue.IsValid() && DocumentValue->Type == EJson::Null) {
      return Result;
    }
    if (!DocumentValue.IsValid() || DocumentValue->Type != EJson::Object ||
        !TryParseDocument(DocumentValue->AsObject(), Result.Document)) {
      Result.Document = FRedwoodAccountDocument();
      Result.Error = MalformedReplyError;
      return Result;
    }
    Result.bFound = true;
    return Result;
  }

  FRedwoodPutAccountDocumentOutput ParsePutReply(
    const TArray<TSharedPtr<FJsonValue>> &Response
  ) {
    FRedwoodPutAccountDocumentOutput Result;
    const TSharedPtr<FJsonObject> Reply = GetReplyObject(Response);
    if (!Reply.IsValid() ||
        !TryGetStrictString(Reply, TEXT("error"), Result.Error) ||
        !TryGetStrictBool(Reply, TEXT("conflict"), Result.bConflict) ||
        !TryGetRevision(Reply, TEXT("revision"), Result.Revision) ||
        !TryGetStrictBool(Reply, TEXT("tooLarge"), Result.bTooLarge)) {
      Result = FRedwoodPutAccountDocumentOutput();
      Result.Error = MalformedReplyError;
    }
    return Result;
  }

  FString GetOfflinePath(const FString &PlayerId, const FString &Key) {
    FString SavePath =
      FPaths::ProjectSavedDir() / TEXT("Persistence") / TEXT("AccountDocuments");
    FPaths::NormalizeFilename(SavePath);
    return SavePath / PlayerId / (Key + TEXT(".json"));
  }

  // A missing file is "no document". A main file that exists but cannot be
  // read or parsed is an error: treating it as missing would let the next put
  // overwrite a document that only needs repair.
  // A put writes the temp file and moves it over the main file. A crash in the
  // move can leave only the temp file, which then holds the whole new
  // document. A crash while the temp file is written, with no main file, can
  // leave a torn temp file: that put never finished, so there is no document.
  FRedwoodGetAccountDocumentOutput ReadOffline(
    const FString &PlayerId, const FString &Key
  ) {
    FRedwoodGetAccountDocumentOutput Result;
    const FString FileName = GetOfflinePath(PlayerId, Key);
    const FString TempFileName = GetOfflineTempPath(PlayerId, Key);
    IFileManager &FileManager = IFileManager::Get();

    if (FileManager.FileExists(*FileName)) {
      if (ReadDocumentFile(FileName, Result.Document) != EReadFileResult::Read) {
        Result.Document = FRedwoodAccountDocument();
        Result.Error = TEXT("Failed to read account document file");
        return Result;
      }
      Result.bFound = true;
      return Result;
    }

    if (!FileManager.FileExists(*TempFileName)) {
      return Result;
    }

    switch (ReadDocumentFile(TempFileName, Result.Document)) {
      case EReadFileResult::Read:
        Result.bFound = true;
        // If this move fails, the next read finds the temp file again.
        MoveOverTarget(FileName, TempFileName);
        break;
      case EReadFileResult::Unparseable:
        Result.Document = FRedwoodAccountDocument();
        FileManager.Delete(*TempFileName);
        break;
      case EReadFileResult::Unreadable:
        Result.Error = TEXT("Failed to read account document file");
        break;
    }
    return Result;
  }

  FRedwoodPutAccountDocumentOutput WriteOffline(
    const FString &PlayerId,
    const FString &Key,
    const TSharedRef<FJsonObject> &Data,
    int32 ExpectedRevision
  ) {
    FRedwoodPutAccountDocumentOutput Output;

    const FRedwoodGetAccountDocumentOutput Current = ReadOffline(PlayerId, Key);
    if (!Current.Error.IsEmpty()) {
      Output.Error = Current.Error;
      return Output;
    }

    const int32 CurrentRevision = Current.bFound ? Current.Document.Revision : 0;
    if (CurrentRevision != ExpectedRevision) {
      Output.bConflict = true;
      return Output;
    }
    if (CurrentRevision == MAX_int32) {
      Output.Error = TEXT("Account document revision is at its maximum");
      return Output;
    }
    const int32 NewRevision = CurrentRevision + 1;

    TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
    FileObject->SetNumberField(TEXT("revision"), NewRevision);
    FileObject->SetObjectField(TEXT("data"), Data);
    FString FileContents;
    TSharedRef<TJsonWriter<TCHAR>> JsonWriter =
      TJsonWriterFactory<TCHAR>::Create(&FileContents);
    FJsonSerializer::Serialize(FileObject, JsonWriter);

    // Write a temp file and move it over the main file. A crash in a direct
    // write would leave a truncated main file, which reads back as a corrupt
    // document that every later put refuses to overwrite.
    const FString FileName = GetOfflinePath(PlayerId, Key);
    const FString TempFileName = GetOfflineTempPath(PlayerId, Key);
    IFileManager &FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*FPaths::GetPath(FileName), true);
    if (!FFileHelper::SaveStringToFile(FileContents, *TempFileName)) {
      FileManager.Delete(*TempFileName);
      Output.Error = TEXT("Failed to write account document file");
      return Output;
    }
    if (!MoveOverTarget(FileName, TempFileName)) {
      // The move can fail after it deleted the main file. The temp file is
      // then the only copy, and the next read recovers it, so it is kept. The
      // caller sees an error and must Get before it retries.
      if (FileManager.FileExists(*FileName)) {
        FileManager.Delete(*TempFileName);
      }
      Output.Error = TEXT("Failed to move account document file");
      return Output;
    }

    Output.Revision = NewRevision;
    return Output;
  }
} // namespace RedwoodAccountDocuments
