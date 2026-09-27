// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodClosingSockets.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

namespace {
  // One fallback release, counted from the close, when the library never
  // reports it. Longer than the library's 3 s close timer.
  constexpr double SafetyReleaseSeconds = 5.0;

  struct FClosingSocket {
    uint64 Id = 0;
    TSharedPtr<FSocketIONative> Socket;
    FTSTicker::FDelegateHandle SafetyRelease;
  };

  // Set first at exit. The task graph shuts down after it, and a close report
  // dispatched then would crash the socket thread.
  bool bClosingSocketsExiting = false;
  // Makes the socket thread's flag check and its dispatch one step against
  // the exit's store. Without it, the socket thread can read false, stall,
  // and dispatch after the task graph shut down. OnPreExit runs before that
  // shutdown, so a dispatch that got the lock first still finds the graph
  // alive. Never held around ReleaseHeld: the game thread must not wait on
  // the socket thread. Leaked, because a kept socket's timer can take it
  // after static destruction.
  FCriticalSection &ClosingSocketsExitLock() {
    static FCriticalSection *Lock = new FCriticalSection();
    return *Lock;
  }

  // Game thread only. Its sockets move to a leaked array at exit.
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

  // At exit, a socket still held is never freed. Its close timer can still
  // run before the process ends: the socket is alive, and with the flag set
  // its close report dispatches nothing to the task graph, which shuts down
  // after this. The callback is not cleared here, because the timer thread
  // can read it at any time. The heap array is leaked on purpose: the
  // process ends the socket thread. It is never a static TSharedPtr released
  // at atexit, a known crash class.
  void KeepAllAtExit() {
    {
      FScopeLock Lock(&ClosingSocketsExitLock());
      bClosingSocketsExiting = true;
    }
    TArray<TSharedPtr<FSocketIONative>> *Kept =
      new TArray<TSharedPtr<FSocketIONative>>();
    for (FClosingSocket &Entry : ClosingSockets()) {
      FTSTicker::GetCoreTicker().RemoveTicker(Entry.SafetyRelease);
      Kept->Add(MoveTemp(Entry.Socket));
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
    FCoreDelegates::OnPreExit.AddStatic(&KeepAllAtExit);
  }

  static uint64 NextId = 0;
  const uint64 Id = ++NextId;
  // The callback runs on the socket's own thread, not through a queued
  // game-thread lambda, so the exit flag can stop it; the socket's other
  // callbacks are already cleared. The release waits for a later game-thread
  // task.
  Socket->bCallbackOnGameThread = false;
  Socket->OnNamespaceDisconnectedCallback = [Id](const FString &) {
    FScopeLock Lock(&ClosingSocketsExitLock());
    if (!bClosingSocketsExiting) {
      AsyncTask(ENamedThreads::GameThread, [Id]() { ReleaseHeld(Id, false); });
    }
  };

  FClosingSocket Entry;
  Entry.Id = Id;
  Entry.Socket = MoveTemp(Socket);
  const double SafetyDelay = FMath::Max(
    0.0, CloseStartedAt + SafetyReleaseSeconds - FPlatformTime::Seconds()
  );
  Entry.SafetyRelease = FTSTicker::GetCoreTicker().AddTicker(
    FTickerDelegate::CreateLambda([Id](float) {
      ReleaseHeld(Id, true);
      return false;
    }),
    static_cast<float>(SafetyDelay)
  );
  ClosingSockets().Add(MoveTemp(Entry));
}

int32 RedwoodClosingSockets::NumHeld() {
  return ClosingSockets().Num();
}
