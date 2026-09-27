// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#include "RedwoodClosingSockets.h"

#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "Misc/CoreDelegates.h"
#include "SocketIOClient.h"
#include "SocketIONative.h"

namespace {
  struct FClosingSocket {
    uint64 Id = 0;
    TSharedPtr<FSocketIONative> Socket;
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

  // Every socket released after the exit started, and every held one then.
  // Leaked on purpose: the process ends the socket threads, and a static
  // array would free a closing socket at static destruction.
  TArray<TSharedPtr<FSocketIONative>> &KeptAtExit() {
    static TArray<TSharedPtr<FSocketIONative>> *Kept =
      new TArray<TSharedPtr<FSocketIONative>>();
    return *Kept;
  }

  // Game thread only. Its sockets move to KeptAtExit at exit.
  TArray<FClosingSocket> &ClosingSockets() {
    static TArray<FClosingSocket> Sockets;
    return Sockets;
  }

  void ReleaseNow(TSharedPtr<FSocketIONative> Socket) {
    ISocketIOClientModule::Get().ReleaseNativePointer(Socket);
  }

  void ReleaseHeld(uint64 Id) {
    TArray<FClosingSocket> &Sockets = ClosingSockets();
    const int32 Index = Sockets.IndexOfByPredicate(
      [Id](const FClosingSocket &Entry) { return Entry.Id == Id; }
    );
    if (Index == INDEX_NONE) {
      return;
    }
    FClosingSocket Entry = MoveTemp(Sockets[Index]);
    Sockets.RemoveAtSwap(Index);
    ReleaseNow(MoveTemp(Entry.Socket));
  }

  // At exit, a socket still held is never freed. Its close timer can still
  // run before the process ends: the socket is alive, and with the flag set
  // its close report dispatches nothing to the task graph, which shuts down
  // after this. The callback is not cleared here, because the timer thread
  // can read it at any time. It is never a static TSharedPtr released at
  // atexit, a known crash class.
  void KeepAllAtExit() {
    {
      FScopeLock Lock(&ClosingSocketsExitLock());
      bClosingSocketsExiting = true;
    }
    for (FClosingSocket &Entry : ClosingSockets()) {
      KeptAtExit().Add(MoveTemp(Entry.Socket));
    }
    ClosingSockets().Empty();
  }
}

void RedwoodClosingSockets::Release(
  TSharedPtr<FSocketIONative> Socket, bool bCloseTimerPending
) {
  if (!Socket.IsValid()) {
    return;
  }
  // After the exit started (a subsystem deinitialized late), nothing may
  // reach the static array, and a close report could not release it. Read
  // without the lock: the game thread is also the one that sets it.
  if (bClosingSocketsExiting) {
    KeptAtExit().Add(MoveTemp(Socket));
    return;
  }
  if (!bCloseTimerPending) {
    ReleaseNow(MoveTemp(Socket));
    return;
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
      AsyncTask(ENamedThreads::GameThread, [Id]() { ReleaseHeld(Id); });
    }
  };

  FClosingSocket Entry;
  Entry.Id = Id;
  Entry.Socket = MoveTemp(Socket);
  ClosingSockets().Add(MoveTemp(Entry));
}

void RedwoodClosingSockets::BindExitHook() {
  FCoreDelegates::OnPreExit.AddStatic(&KeepAllAtExit);
}

int32 RedwoodClosingSockets::NumHeld() {
  return ClosingSockets().Num();
}
