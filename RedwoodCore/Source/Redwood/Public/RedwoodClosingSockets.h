// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

#pragma once

#include "CoreMinimal.h"

class FSocketIONative;

// HollowedOath#2999. A close that the client asks for starts a timer in the
// socket library (sio::socket::impl::close) that runs on_close 3 s later on
// the socket's own thread. It keeps a raw pointer to the socket, and a
// cancelled wait still calls it, so freeing the socket before it runs is a
// use-after-free. A socket with that close in progress is held here until
// the library reports the namespace closed, which the timer does. There is
// no fallback release by time: a stalled socket thread can still have the
// close queued. A socket whose close is never reported is held until exit,
// and at exit it is kept, never freed.
namespace RedwoodClosingSockets {
  // Releases Socket through the plugin, now, or once its close timer ran
  // when bCloseTimerPending. Game thread only.
  REDWOOD_API void Release(
    TSharedPtr<FSocketIONative> Socket, bool bCloseTimerPending
  );

  // Keeps every socket from the start of the exit on. Called once, at module
  // startup, so it is bound before any socket is released.
  void BindExitHook();

  // The sockets held now. For tests.
  REDWOOD_API int32 NumHeld();
}
