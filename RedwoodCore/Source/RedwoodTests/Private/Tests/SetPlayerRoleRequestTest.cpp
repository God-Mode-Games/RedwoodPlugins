// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the two halves of URedwoodServerGameSubsystem::RequestPlayerRoleChange,
// the game-server side of the in-game /gm command:
//   1. MakeSetPlayerRolePayload puts an explicit null in roleKey when the key
//      is empty. The backend declares roleKey as a field that must be present,
//      so a key that is left out is refused as a malformed request, and the
//      command to clear a role would never work.
//   2. AnswerSetPlayerRole reports failure for an answer that is not an
//      object. AsObject would turn such an answer into a valid EMPTY object,
//      which reads as "no error" -- that is, as a role change that never
//      happened.
//   3. AnswerSetPlayerRole refuses an answer with no `error` field. The
//      backend puts an error on every answer, so an object without one is not
//      one of its answers, and reading the absent field as an empty error
//      would report a role change that never happened.
//   4. AnswerSetPlayerRole reads the committed tier, and keeps 0 for a number
//      that cannot become an int32.
//   5. SetPlayerRoleEventName still holds the wire name the backend declares.
//      The backend keeps its own copy of the string, and a silent drift on
//      either side would turn the whole command off.
// The socket itself is not covered here: the RedwoodBackend test harness
// covers the relay, and the game covers what a player reads.
// An upstream merge must keep these APIs or update this file in lockstep.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RedwoodServerGameSubsystem.h"

// No UE equivalent exists for the IEEE special values, and the engine itself
// reaches for <limits> in the same situation.
#include <limits>

namespace {
  // What one answer told the caller.
  struct FRoleAnswer {
    bool bOk = false;
    int32 NewRole = 0;
    FString Error;
  };

  FRoleAnswer AnswerFor(const TSharedPtr<FJsonValue> &ResponseValue) {
    TArray<TSharedPtr<FJsonValue>> Response;
    Response.Add(ResponseValue);

    FRoleAnswer Answer;
    URedwoodServerGameSubsystem::AnswerSetPlayerRole(
      Response,
      FRedwoodSetPlayerRoleOutputDelegate::CreateLambda(
        [&Answer](bool bOk, int32 NewRole, const FString &Error) {
          Answer.bOk = bOk;
          Answer.NewRole = NewRole;
          Answer.Error = Error;
        }
      )
    );

    return Answer;
  }

  FRoleAnswer AnswerForObject(const TSharedPtr<FJsonObject> &Object) {
    return AnswerFor(MakeShared<FJsonValueObject>(Object));
  }

  // Every refusal answer carries an error and no role; every success answer
  // carries an empty error and the committed tier.
  TSharedPtr<FJsonObject> MakeSuccessObj(const double RoleValue) {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("error"), TEXT(""));
    Obj->SetField(TEXT("role"), MakeShared<FJsonValueNumber>(RoleValue));
    return Obj;
  }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSetPlayerRolePayloadTest,
  "Redwood.SetPlayerRole.Payload",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSetPlayerRolePayloadTest::RunTest(const FString &Parameters) {
  // The backend keeps its own copy of this string, so the two can drift apart
  // with nothing to show for it. Pinned here, exactly as it is written in
  // RedwoodBackend packages/common/src/interfaces.ts.
  TestEqual(
    TEXT("The wire name has not drifted"),
    FString(URedwoodServerGameSubsystem::SetPlayerRoleEventName),
    TEXT("realm:servers:set-player-role:game-server-to-sidecar")
  );

  TSharedPtr<FJsonObject> Granted =
    URedwoodServerGameSubsystem::MakeSetPlayerRolePayload(
      TEXT("target-1"), TEXT("gm"), TEXT("actor-1")
    );

  TestEqual(
    TEXT("The target player is named"),
    Granted->GetStringField(TEXT("targetPlayerId")),
    TEXT("target-1")
  );
  TestEqual(
    TEXT("The player who asked is named"),
    Granted->GetStringField(TEXT("actorPlayerId")),
    TEXT("actor-1")
  );
  TestEqual(
    TEXT("A role key goes out as a string"),
    Granted->GetStringField(TEXT("roleKey")),
    TEXT("gm")
  );

  // The clear case. The field must be PRESENT and null; a payload without it
  // is refused by the backend before it reaches the director.
  TSharedPtr<FJsonObject> Cleared =
    URedwoodServerGameSubsystem::MakeSetPlayerRolePayload(
      TEXT("target-1"), FString(), TEXT("actor-1")
    );

  const TSharedPtr<FJsonValue> ClearedKey =
    Cleared->TryGetField(TEXT("roleKey"));
  TestTrue(TEXT("An empty key still sends the field"), ClearedKey.IsValid());
  if (ClearedKey.IsValid()) {
    TestTrue(
      TEXT("An empty key sends an explicit null"),
      ClearedKey->Type == EJson::Null
    );
  }

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSetPlayerRoleAnswerTest,
  "Redwood.SetPlayerRole.Answer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSetPlayerRoleAnswerTest::RunTest(const FString &Parameters) {
  const FRoleAnswer Granted = AnswerForObject(MakeSuccessObj(75.0));
  TestTrue(TEXT("An answer with no error succeeds"), Granted.bOk);
  TestEqual(TEXT("The committed tier is carried"), Granted.NewRole, 75);
  TestEqual(TEXT("A success carries no error"), Granted.Error, FString());

  // A tier this build predates must survive as its raw number: the game
  // compares tiers with >= rather than mapping onto a known name.
  TestEqual(
    TEXT("An unknown tier is carried unchanged"),
    AnswerForObject(MakeSuccessObj(60.0)).NewRole,
    60
  );

  // A refusal. The string goes back word for word, because the game turns it
  // into what the player reads.
  TSharedPtr<FJsonObject> RefusedObj = MakeShared<FJsonObject>();
  RefusedObj->SetStringField(TEXT("error"), TEXT("rank refused"));

  const FRoleAnswer Refused = AnswerForObject(RefusedObj);
  TestFalse(TEXT("An answer with an error fails"), Refused.bOk);
  TestEqual(
    TEXT("The backend words are not changed"),
    Refused.Error,
    TEXT("rank refused")
  );
  TestEqual(TEXT("A refusal carries no tier"), Refused.NewRole, 0);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodSetPlayerRoleBadAnswerTest,
  "Redwood.SetPlayerRole.BadAnswer",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodSetPlayerRoleBadAnswerTest::RunTest(const FString &Parameters) {
  // An answer that is not an object at all. AsObject would make a valid empty
  // object out of each of these, and an empty object has no error -- so the
  // caller would be told a role change happened.
  const FRoleAnswer FromNull = AnswerFor(MakeShared<FJsonValueNull>());
  TestFalse(TEXT("A null answer fails"), FromNull.bOk);
  TestFalse(TEXT("A null answer says why"), FromNull.Error.IsEmpty());

  const FRoleAnswer FromString =
    AnswerFor(MakeShared<FJsonValueString>(TEXT("nope")));
  TestFalse(TEXT("A string answer fails"), FromString.bOk);

  // No answer values at all, and an answer value that is a dead pointer.
  TArray<TSharedPtr<FJsonValue>> Empty;
  FRoleAnswer FromEmpty;
  URedwoodServerGameSubsystem::AnswerSetPlayerRole(
    Empty,
    FRedwoodSetPlayerRoleOutputDelegate::CreateLambda(
      [&FromEmpty](bool bOk, int32 NewRole, const FString &Error) {
        FromEmpty.bOk = bOk;
        FromEmpty.Error = Error;
      }
    )
  );
  TestFalse(TEXT("An empty answer fails"), FromEmpty.bOk);
  TestFalse(TEXT("An empty answer says why"), FromEmpty.Error.IsEmpty());

  const FRoleAnswer FromInvalid = AnswerFor(nullptr);
  TestFalse(TEXT("An answer with no value fails"), FromInvalid.bOk);

  // An object with no error field. The backend puts an error on every answer,
  // so this is not one of its answers. Reading the absent field as an empty
  // error would report a role change that never happened.
  const FRoleAnswer FromBareObject = AnswerForObject(MakeShared<FJsonObject>());
  TestFalse(TEXT("An object with no error field fails"), FromBareObject.bOk);
  TestFalse(
    TEXT("An object with no error field says why"),
    FromBareObject.Error.IsEmpty()
  );
  TestEqual(
    TEXT("An object with no error field carries no tier"),
    FromBareObject.NewRole,
    0
  );

  // The same shape, but with a tier. The missing error still decides.
  TSharedPtr<FJsonObject> RoleOnlyObj = MakeShared<FJsonObject>();
  RoleOnlyObj->SetField(TEXT("role"), MakeShared<FJsonValueNumber>(75.0));
  TestFalse(
    TEXT("A tier without an error field fails"),
    AnswerForObject(RoleOnlyObj).bOk
  );

  // A tier that cannot become an int32. Narrowing these with a bare
  // static_cast is undefined behaviour, so the guard keeps 0 instead. The
  // change is still reported as made: the error string decides that, and a
  // committed change reported as a failure makes an operator ask twice.
  const FRoleAnswer TooBig = AnswerForObject(MakeSuccessObj(1.0e18));
  TestTrue(TEXT("An unusable tier still reports the change"), TooBig.bOk);
  TestEqual(TEXT("An unusable tier stays 0"), TooBig.NewRole, 0);

  TestEqual(
    TEXT("NaN stays 0"),
    AnswerForObject(
      MakeSuccessObj(std::numeric_limits<double>::quiet_NaN())
    ).NewRole,
    0
  );

  // One past MAX_int32. Narrowing this with a bare static_cast is undefined
  // behaviour, so before the guard the result was whatever the target's
  // float-to-int instruction happened to give.
  TestEqual(
    TEXT("One past MAX_int32 stays 0"),
    AnswerForObject(MakeSuccessObj(2147483648.0)).NewRole,
    0
  );

  // Both exact bounds are representable as a double and must pass through:
  // the guard rejects what is out of range, not what is merely large.
  TestEqual(
    TEXT("MAX_int32 passes through"),
    AnswerForObject(MakeSuccessObj(2147483647.0)).NewRole,
    MAX_int32
  );
  TestEqual(
    TEXT("MIN_int32 passes through"),
    AnswerForObject(MakeSuccessObj(-2147483648.0)).NewRole,
    MIN_int32
  );

  return true;
}
