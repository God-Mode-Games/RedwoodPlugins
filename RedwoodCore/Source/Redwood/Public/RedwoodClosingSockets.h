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
// the library reports the namespace closed, which the timer does.
namespace RedwoodClosingSockets {
  // How long after the close the library's timer runs.
  constexpr double CloseTimerSeconds = 3.0;

  // Releases Socket through the plugin, now or once its close timer ran.
  // CloseStartedAt is the FPlatformTime::Seconds() of the close, or negative
  // when no close timer is pending. Game thread only.
  REDWOOD_API void Release(
    TSharedPtr<FSocketIONative> Socket, double CloseStartedAt
  );

  // The sockets held now. For tests.
  REDWOOD_API int32 NumHeld();
}
