// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart. It pins the field
// names this plugin reads out of the director "director:friends:request-alert" push, so a
// change to the parser cannot stop reading "fromPlayerId" or "fromPlayerNickname" without a
// test failure.
//
// These tests hold the plugin side only. They build their JSON from the same names the parser
// reads, so a rename in the backend leaves them green. The agreement between the two
// repositories is held by the RequestAlert schema in the backend,
// packages/common/src/interfaces.ts: keep the names below equal to the names there. Each of the
// two names fails in its own way. If the backend renames "fromPlayerId", the listener in
// RedwoodClientInterface.cpp drops every alert and logs a warning, which is the symptom to look
// for. If the backend renames "fromPlayerNickname", every alert still arrives, but with an
// empty name and no log line, because the name is optional.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "RedwoodCommonGameSubsystem.h"
#include "Types/RedwoodTypesPlayers.h"

namespace {
  // The alert names the player it is for, then the player who sent the request.
  TSharedPtr<FJsonObject> MakeAlertObj() {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("playerId"), TEXT("receiver-1"));
    Obj->SetStringField(TEXT("fromPlayerId"), TEXT("sender-1"));
    Obj->SetStringField(TEXT("fromPlayerNickname"), TEXT("Sender"));
    return Obj;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodFriendRequestAlertFieldsTest,
  "Redwood.FriendRequestAlert.Fields",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodFriendRequestAlertFieldsTest::RunTest(const FString &Parameters) {
  const FRedwoodPlayer Requester =
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(MakeAlertObj());

  TestEqual(
    TEXT("The requester id comes from fromPlayerId"),
    Requester.PlayerId,
    TEXT("sender-1")
  );
  TestEqual(
    TEXT("The requester name comes from fromPlayerNickname"),
    Requester.Nickname,
    TEXT("Sender")
  );

  // The game shows a pushed request in the same list that
  // ListFriends(PendingReceived) fills, so the pushed row must carry that state
  // itself; the push has no field that says so.
  TestTrue(
    TEXT("The requester is a received request"),
    Requester.FriendshipState == ERedwoodFriendListType::PendingReceived
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodFriendRequestAlertNoNicknameTest,
  "Redwood.FriendRequestAlert.NoNickname",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodFriendRequestAlertNoNicknameTest::RunTest(const FString &Parameters
) {
  // The nickname is a convenience. A player who has no nickname sends nothing,
  // or an empty text. The request must still arrive, with an empty name.
  TSharedPtr<FJsonObject> AbsentObj = MakeAlertObj();
  AbsentObj->RemoveField(TEXT("fromPlayerNickname"));

  const FRedwoodPlayer FromAbsent =
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(AbsentObj);
  TestEqual(
    TEXT("An absent name gives an empty name"), FromAbsent.Nickname, FString()
  );
  TestEqual(
    TEXT("An absent name keeps the requester id"),
    FromAbsent.PlayerId,
    TEXT("sender-1")
  );
  TestTrue(
    TEXT("An absent name keeps the received state"),
    FromAbsent.FriendshipState == ERedwoodFriendListType::PendingReceived
  );

  TSharedPtr<FJsonObject> EmptyObj = MakeAlertObj();
  EmptyObj->SetStringField(TEXT("fromPlayerNickname"), TEXT(""));
  TestEqual(
    TEXT("An empty name gives an empty name"),
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(EmptyObj).Nickname,
    FString()
  );

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodFriendRequestAlertBadPayloadTest,
  "Redwood.FriendRequestAlert.BadPayload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodFriendRequestAlertBadPayloadTest::RunTest(const FString &Parameters
) {
  // A push that is not an object does not give an empty pointer: FJsonValue::AsObject gives
  // back a shared empty object, and writes to LogJson -- a warning for a null value, an error
  // for any other value that is not an object. That empty object is the payload the listener
  // sees, so it is the first case here.
  const FRedwoodPlayer FromEmptyObject =
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(
      MakeShared<FJsonObject>()
    );
  TestEqual(
    TEXT("An empty push gives no requester id"),
    FromEmptyObject.PlayerId,
    FString()
  );

  // The parser also takes an empty pointer, which a direct caller can still pass.
  const FRedwoodPlayer FromInvalid =
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(nullptr);
  TestEqual(
    TEXT("A bad payload gives no requester id"), FromInvalid.PlayerId, FString()
  );

  TSharedPtr<FJsonObject> NoSenderObj = MakeShared<FJsonObject>();
  NoSenderObj->SetStringField(TEXT("playerId"), TEXT("receiver-1"));
  TestEqual(
    TEXT("A push with no sender gives no requester id"),
    URedwoodCommonGameSubsystem::ParseFriendRequestAlert(NoSenderObj).PlayerId,
    FString()
  );

  return true;
}
