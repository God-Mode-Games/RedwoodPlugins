// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodClosingSockets.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

namespace {
  // At exit, the wait past the timer, so its on_close has finished.
  constexpr double ExitMarginSeconds = 0.25;
  // One fallback release when the library never reports the close. Longer
  // than the timer.
  constexpr float SafetyReleaseSeconds = 5.0f;

  struct FClosingSocket {
    uint64 Id = 0;
    TSharedPtr<FSocketIONative> Socket;
    double TimerDoneAt = 0.0;
    FTSTicker::FDelegateHandle SafetyRelease;
  };

  // Emptied at exit, so no socket outlives the plugin.
  TArray<FClosingSocket> &ClosingSockets() {
    static TArray<FClosingSocket> Sockets;
    return Sockets;
  }

  void ReleaseNow(TSharedPtr<FSocketIONative> Socket) {
    ISocketIOClientModule::Get().ReleaseNativePointer(Socket);
  }

  void ReleaseHeld(uint64 Id, bool bFromSafetyTicker) {
    TArray<FClosingSocket> &Sockets = ClosingSockets();
    const int32 Index = Sockets.IndexOfByPredicate(
      [Id](const FClosingSocket &Entry) { return Entry.Id == Id; }
    );
    if (Index == INDEX_NONE) {
      return;
    }
    FClosingSocket Entry = MoveTemp(Sockets[Index]);
    Sockets.RemoveAtSwap(Index);
    if (!bFromSafetyTicker) {
      FTSTicker::GetCoreTicker().RemoveTicker(Entry.SafetyRelease);
    }
    ReleaseNow(MoveTemp(Entry.Socket));
  }

  // Nothing ticks at exit. Each wait ends at its own timer, so the whole
  // wait is at most one timer long.
  void ReleaseAllAtExit() {
    for (FClosingSocket &Entry : ClosingSockets()) {
      const double Wait = Entry.TimerDoneAt - FPlatformTime::Seconds();
      if (Wait > 0.0) {
        FPlatformProcess::Sleep(static_cast<float>(Wait));
      }
      FTSTicker::GetCoreTicker().RemoveTicker(Entry.SafetyRelease);
      ReleaseNow(MoveTemp(Entry.Socket));
    }
    ClosingSockets().Empty();
  }
}

void RedwoodClosingSockets::Release(
  TSharedPtr<FSocketIONative> Socket, double CloseStartedAt
) {
  if (!Socket.IsValid()) {
    return;
  }
  if (CloseStartedAt < 0.0) {
    ReleaseNow(MoveTemp(Socket));
    return;
  }

  static bool bExitHookBound = false;
  if (!bExitHookBound) {
    bExitHookBound = true;
    FCoreDelegates::OnPreExit.AddStatic(&ReleaseAllAtExit);
  }

  static uint64 NextId = 0;
  const uint64 Id = ++NextId;
  // The library runs this callback from the socket; the release waits for a
  // later game-thread task.
  Socket->OnNamespaceDisconnectedCallback = [Id](const FString &) {
    AsyncTask(ENamedThreads::GameThread, [Id]() { ReleaseHeld(Id, false); });
  };

  FClosingSocket Entry;
  Entry.Id = Id;
  Entry.Socket = MoveTemp(Socket);
  Entry.TimerDoneAt = CloseStartedAt + CloseTimerSeconds + ExitMarginSeconds;
  Entry.SafetyRelease = FTSTicker::GetCoreTicker().AddTicker(
    FTickerDelegate::CreateLambda([Id](float) {
      ReleaseHeld(Id, true);
      return false;
    }),
    SafetyReleaseSeconds
  );
  ClosingSockets().Add(MoveTemp(Entry));
}

int32 RedwoodClosingSockets::NumHeld() {
  return ClosingSockets().Num();
}
