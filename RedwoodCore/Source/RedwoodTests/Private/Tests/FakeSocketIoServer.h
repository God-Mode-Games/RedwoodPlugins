// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.
// A minimal websocket server on a raw TCP socket, so the real socket.io
// client and websocketpp run the whole handshake and close. Shared by the
// close and in-flight tests.

#pragma once

#include "CoreMinimal.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace RedwoodFakeSocketIo {
  // Long enough for a loaded build machine, short enough to fail fast.
  const FTimespan StepTimeout = FTimespan::FromSeconds(5.0);
  constexpr uint32 StepTimeoutMs = 5000;

  constexpr uint32 LoopbackIp = 0x7F000001;
  constexpr uint16 NormalCloseCode = 1000;

  // The library queues its reports to the game thread, which a test holds.
  // The pump runs those tasks at 10 Hz, the fastest this project allows a
  // wait to check, until the test's condition holds or the step times out.
  constexpr float PumpIntervalSeconds = 0.1f;

  inline bool PumpGameThreadUntil(TFunctionRef<bool()> Done) {
    const double Deadline =
      FPlatformTime::Seconds() + StepTimeout.GetTotalSeconds();
    while (true) {
      FTaskGraphInterface::Get().ProcessThreadUntilIdle(
        ENamedThreads::GameThread
      );
      if (Done()) {
        return true;
      }
      if (FPlatformTime::Seconds() > Deadline) {
        return false;
      }
      FPlatformProcess::Sleep(PumpIntervalSeconds);
    }
  }

  // RFC 6455 section 1.3.
  inline const TCHAR *const WebSocketAcceptGuid = TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

  // An engine.io v4 open packet, so the client starts its socket.io session.
  inline const char *const EngineIoOpenPacket =
    "0{\"sid\":\"test\",\"upgrades\":[],\"pingInterval\":25000,"
    "\"pingTimeout\":20000,\"maxPayload\":1000000}";

  // Accepts the client's "/" namespace. Unanswered, its 20 s connect timer
  // keeps the socket thread alive, and SyncDisconnect waits for it.
  inline const char *const SocketIoConnectPacket = "40{\"sid\":\"test\"}";

  inline TArray<uint8> AnsiBytes(const char *Text) {
    return TArray<uint8>(
      reinterpret_cast<const uint8 *>(Text), FCStringAnsi::Strlen(Text)
    );
  }

  inline bool SendAll(FSocket *Socket, const TArray<uint8> &Bytes) {
    int32 Sent = 0;
    return Socket->Send(Bytes.GetData(), Bytes.Num(), Sent) &&
      Sent == Bytes.Num();
  }

  // Reads whatever arrives next, so the test knows the client has spoken.
  inline bool ReadSome(FSocket *Socket, TArray<uint8> &OutBytes) {
    if (!Socket->Wait(ESocketWaitConditions::WaitForRead, StepTimeout)) {
      return false;
    }
    uint8 Buffer[1024];
    int32 Read = 0;
    if (!Socket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0) {
      return false;
    }
    OutBytes.Append(Buffer, Read);
    return true;
  }

  // RFC 6455 section 5.2: a 7-bit length, or 126 and a 16-bit length.
  constexpr int32 SevenBitLengthLimit = 126;

  // Server frames are not masked, and these payloads fit a 16-bit length.
  inline TArray<uint8> MakeFrame(uint8 Opcode, const TArray<uint8> &Payload) {
    check(Payload.Num() <= MAX_uint16);
    TArray<uint8> Frame = {static_cast<uint8>(0x80 | Opcode)};
    if (Payload.Num() < SevenBitLengthLimit) {
      Frame.Add(static_cast<uint8>(Payload.Num()));
    } else {
      Frame.Add(static_cast<uint8>(SevenBitLengthLimit));
      Frame.Add(static_cast<uint8>(Payload.Num() >> 8));
      Frame.Add(static_cast<uint8>(Payload.Num() & 0xFF));
    }
    Frame.Append(Payload);
    return Frame;
  }

  struct FFakeSocketIoServer {
    ISocketSubsystem *Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket *Listener = nullptr;
    FSocket *Connection = nullptr;
    int32 Port = 0;

    bool Listen() {
      Listener = Subsystem->CreateSocket(
        NAME_Stream, TEXT("RedwoodFakeSocketIo"), FNetworkProtocolTypes::IPv4
      );
      TSharedRef<FInternetAddr> Address =
        Subsystem->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
      Address->SetIp(LoopbackIp);
      Address->SetPort(0);
      if (!Listener || !Listener->Bind(*Address) || !Listener->Listen(1)) {
        return false;
      }
      Port = Listener->GetPortNo();
      return true;
    }

    bool WaitForConnection() const {
      return Listener->Wait(ESocketWaitConditions::WaitForRead, StepTimeout);
    }

    // Accepts the websocket upgrade, opens the engine.io session, waits for
    // the client's namespace connect and accepts it.
    bool AcceptSession() {
      if (!WaitForConnection()) {
        return false;
      }
      Connection = Listener->Accept(TEXT("RedwoodFakeSocketIoConnection"));
      if (!Connection) {
        return false;
      }

      TArray<uint8> Request;
      FString RequestText;
      while (!RequestText.Contains(TEXT("\r\n\r\n"))) {
        if (!ReadSome(Connection, Request)) {
          return false;
        }
        const FUTF8ToTCHAR Converted(
          reinterpret_cast<const UTF8CHAR *>(Request.GetData()), Request.Num()
        );
        RequestText = FString(Converted.Length(), Converted.Get());
      }

      const FString KeyHeader = TEXT("Sec-WebSocket-Key:");
      const int32 KeyStart = RequestText.Find(KeyHeader);
      if (KeyStart == INDEX_NONE) {
        return false;
      }
      const int32 ValueStart = KeyStart + KeyHeader.Len();
      const int32 ValueEnd = RequestText.Find(
        TEXT("\r\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart
      );
      const FString Key =
        RequestText.Mid(ValueStart, ValueEnd - ValueStart).TrimStartAndEnd();

      const FTCHARToUTF8 AcceptSource(*(Key + WebSocketAcceptGuid));
      uint8 Digest[FSHA1::DigestSize];
      FSHA1::HashBuffer(AcceptSource.Get(), AcceptSource.Length(), Digest);
      const FString Response = FString::Printf(
        TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n")
        TEXT("Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"),
        *FBase64::Encode(Digest, FSHA1::DigestSize)
      );
      const FTCHARToUTF8 ResponseUtf8(*Response);
      const TArray<uint8> ResponseBytes(
        reinterpret_cast<const uint8 *>(ResponseUtf8.Get()), ResponseUtf8.Length()
      );

      TArray<uint8> ClientPacket;
      return SendAll(Connection, ResponseBytes) &&
        SendAll(Connection, MakeFrame(0x1, AnsiBytes(EngineIoOpenPacket))) &&
        ReadSome(Connection, ClientPacket) &&
        SendAll(Connection, MakeFrame(0x1, AnsiBytes(SocketIoConnectPacket)));
    }

    // Plays the server side of a close handshake: the close frame, the
    // client's echo, then the TCP close, which RFC 6455 gives the server.
    bool CloseNormally() {
      return CloseWithCode(NormalCloseCode);
    }

    // A close with any code, e.g. 1012 "Service Restart", which the Redwood
    // backends send when they stop.
    bool CloseWithCode(uint16 CloseCode) {
      const TArray<uint8> Code = {
        static_cast<uint8>(CloseCode >> 8), static_cast<uint8>(CloseCode & 0xFF)
      };
      TArray<uint8> Echo;
      const bool bClosed =
        SendAll(Connection, MakeFrame(0x8, Code)) && ReadSome(Connection, Echo);
      DropConnection();
      return bClosed;
    }

    // Waits for the next client frame: the request is on the wire.
    bool ReadClientFrame() {
      TArray<uint8> Frame;
      return ReadSome(Connection, Frame);
    }

    // Sends one engine.io/socket.io text packet, e.g. an event: 42["name",{}].
    bool SendText(const char *Packet) {
      return SendAll(Connection, MakeFrame(0x1, AnsiBytes(Packet)));
    }

    void DropConnection() {
      if (Connection) {
        Connection->Close();
        Subsystem->DestroySocket(Connection);
        Connection = nullptr;
      }
    }

    ~FFakeSocketIoServer() {
      DropConnection();
      if (Listener) {
        Listener->Close();
        Subsystem->DestroySocket(Listener);
      }
    }
  };
}
