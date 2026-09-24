// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// Pins the inline error path of the four character friend calls on
// URedwoodClientInterface. The class exists apart from the subsystem so a
// test can build it without a world (RedwoodClientInterface.h header
// comment). A fresh instance has no realm socket, so every call must answer
// inline with "Not connected to Realm.", with or without a selected
// character. The realm guard comes first, as in every realm call of
// RedwoodClientInterface.cpp. The "No character selected." guard comes after
// it, so it needs a live realm; review covers it, not this test.
// The socket path itself has no harness here (the same limit as the
// request-alert listener, fork PR #27); it is checked in PIE with a backend.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RedwoodClientInterface.h"
#include "Types/RedwoodTypesCharacters.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
  FRedwoodCharacterFriendsNotConnectedTest,
  "Redwood.CharacterFriends.NotConnected",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
);

bool FRedwoodCharacterFriendsNotConnectedTest::RunTest(
  const FString &Parameters
) {
  URedwoodClientInterface *Redwood = NewObject<URedwoodClientInterface>();

  FString Error;
  FRedwoodListCharacterFriendsOutputDelegate OnList =
    FRedwoodListCharacterFriendsOutputDelegate::CreateLambda(
      [&Error](const FRedwoodListCharacterFriendsOutput &Output) {
        Error = Output.Error;
      }
    );
  FRedwoodErrorOutputDelegate OnError =
    FRedwoodErrorOutputDelegate::CreateLambda(
      [&Error](const FString &Output) { Error = Output; }
    );

  // TestEqualSensitive: the TCHAR* form of TestEqual compares without case.
  // Clearing after each check makes a call that never answers fail here.
  const auto CheckAll = [&](const TCHAR *Expected, const TCHAR *Case) {
    Redwood->ListCharacterFriends(OnList);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RequestCharacterFriend(TEXT("other-1"), OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RespondToCharacterFriendRequest(TEXT("other-1"), true, OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();

    Redwood->RemoveCharacterFriend(TEXT("other-1"), OnError);
    TestEqualSensitive(Case, *Error, Expected);
    Error.Reset();
  };

  // With no realm and no character, the realm error wins: the realm guard is
  // first.
  CheckAll(TEXT("Not connected to Realm."), TEXT("No realm, no character"));

  // SetSelectedCharacter keeps the id and sends nothing without a realm.
  Redwood->SetSelectedCharacter(TEXT("me-1"));
  CheckAll(TEXT("Not connected to Realm."), TEXT("No realm, a character"));

  return true;
}
