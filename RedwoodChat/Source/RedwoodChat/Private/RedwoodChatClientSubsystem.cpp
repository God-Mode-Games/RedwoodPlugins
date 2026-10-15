// Copyright Incanta Games. All Rights Reserved.

#include "RedwoodChatClientSubsystem.h"
#include "RedwoodChatSettings.h"
#include "RedwoodClientGameSubsystem.h"
#include "RedwoodClientInterface.h"
#include "XmppModule.h"

void URedwoodClientChatSubsystem::Initialize(
  FSubsystemCollectionBase &Collection
) {
  Super::Initialize(Collection);
}

void URedwoodClientChatSubsystem::Deinitialize() {
  Super::Deinitialize();

  FRedwoodXmppModule &Module =
    FModuleManager::GetModuleChecked<FRedwoodXmppModule>("RedwoodXMPP");

  Module.Deinit();
}

void URedwoodClientChatSubsystem::InitializeChatConnection(
  FRedwoodErrorOutputDelegate OnOutput
) {
  if (bInitialized) {
    OnOutput.ExecuteIfBound(TEXT("Already initialized."));
    return;
  }

  URedwoodClientGameSubsystem *GameSubsystem =
    GetGameInstance()->GetSubsystem<URedwoodClientGameSubsystem>();

  if (GameSubsystem) {
    URedwoodClientInterface *ClientInterface =
      GameSubsystem->GetClientInterface();

    if (ClientInterface) {
      if (!ClientInterface->IsDirectorConnected()) {
        OnOutput.ExecuteIfBound(TEXT("Not connected to the Director."));
        return;
      }

      // FORK(hollowed-oath): the chat credentials rode the Director socket,
      // which the client no longer has (it holds one connection, to Bifrost,
      // which carries no text chat event). This XMPP chat was unused and is
      // retired; the chat of Bifrost replaces it. The widgets that call this
      // stay loadable until then, and hear that chat is not available.
      OnOutput.ExecuteIfBound(TEXT("Text chat is not available."));
    } else {
      OnOutput.ExecuteIfBound(TEXT("Redwood Client Interface not found."));
    }
  } else {
    OnOutput.ExecuteIfBound(TEXT("Redwood Client Game Subsystem not found."));
  }
}

bool URedwoodClientChatSubsystem::IsConnected() {
  return XmppPlayerConnection.IsValid() && XmppCharacterConnection.IsValid() &&
    XmppPlayerConnection->GetLoginStatus() == EXmppLoginStatus::LoggedIn &&
    XmppCharacterConnection->GetLoginStatus() == EXmppLoginStatus::LoggedIn;
}

void URedwoodClientChatSubsystem::InitHandlers() {
  if (XmppPlayerConnection->PrivateChat().IsValid()) {
    XmppPlayerConnection->PrivateChat()->OnReceiveChat().AddUObject(
      this, &URedwoodClientChatSubsystem::HandlePlayerPrivateChatReceiveMessage
    );
  }

  if (XmppPlayerConnection->MultiUserChat().IsValid()) {
    XmppPlayerConnection->MultiUserChat()->OnRoomChatReceived().AddUObject(
      this, &URedwoodClientChatSubsystem::HandleRoomChatReceived
    );
    XmppPlayerConnection->MultiUserChat()->OnJoinPrivateRoom().AddUObject(
      this, &URedwoodClientChatSubsystem::HandleJoinPrivateRoom
    );
  }

  if (XmppCharacterConnection->PrivateChat().IsValid()) {
    XmppCharacterConnection->PrivateChat()->OnReceiveChat().AddUObject(
      this,
      &URedwoodClientChatSubsystem::HandleCharacterPrivateChatReceiveMessage
    );
  }

  if (XmppCharacterConnection->MultiUserChat().IsValid()) {
    XmppCharacterConnection->MultiUserChat()->OnRoomChatReceived().AddUObject(
      this, &URedwoodClientChatSubsystem::HandleRoomChatReceived
    );
    XmppCharacterConnection->MultiUserChat()->OnJoinPrivateRoom().AddUObject(
      this, &URedwoodClientChatSubsystem::HandleJoinPrivateRoom
    );
  }
}

void URedwoodClientChatSubsystem::HandlePlayerPrivateChatReceiveMessage(
  const TSharedRef<IXmppConnection> &Connection,
  const FXmppUserJid &InUserJid,
  const TSharedRef<FXmppChatMessage> &Message
) {
  FRedwoodChatIdentity SenderIdentity;
  SenderIdentity.PlayerId = InUserJid.Id;

  FString Body = Message->Body;
  if (Body.IsEmpty()) {
    return;
  }

  TSharedPtr<FJsonObject> MessageObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
  if (!FJsonSerializer::Deserialize(Reader, MessageObject)) {
    return;
  }

  if (!MessageObject->TryGetStringField(TEXT("nickname"), SenderIdentity.Nickname) || SenderIdentity.Nickname.IsEmpty()) {
    return;
  }

  FString MessageText;
  if (!MessageObject->TryGetStringField(TEXT("message"), MessageText) || MessageText.IsEmpty()) {
    return;
  }

  OnPlayerPrivateChatReceived.Broadcast(
    SenderIdentity, Message->Timestamp, MessageText
  );
}

void URedwoodClientChatSubsystem::HandleCharacterPrivateChatReceiveMessage(
  const TSharedRef<IXmppConnection> &Connection,
  const FXmppUserJid &InUserJid,
  const TSharedRef<FXmppChatMessage> &Message
) {
  FRedwoodChatIdentity SenderIdentity;
  SenderIdentity.PlayerId = InUserJid.Id;

  FString Body = Message->Body;
  if (Body.IsEmpty()) {
    return;
  }

  TSharedPtr<FJsonObject> MessageObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
  if (!FJsonSerializer::Deserialize(Reader, MessageObject)) {
    return;
  }

  if (!MessageObject->TryGetStringField(TEXT("nickname"), SenderIdentity.Nickname) || SenderIdentity.Nickname.IsEmpty()) {
    return;
  }

  FString MessageText;
  if (!MessageObject->TryGetStringField(TEXT("message"), MessageText) || MessageText.IsEmpty()) {
    return;
  }

  OnCharacterPrivateChatReceived.Broadcast(
    SenderIdentity, Message->Timestamp, MessageText
  );
}

void URedwoodClientChatSubsystem::HandleJoinPrivateRoom(
  const TSharedRef<IXmppConnection> &Connection,
  bool bSuccess,
  const FString &RoomId,
  const FString &Error
) {
  if (bSuccess) {
    FString RoomTypeString;
    FString RoomIdString;
    RoomId.Split(TEXT("|"), &RoomTypeString, &RoomIdString);

    FRedwoodChatRoomIdentity RoomIdentity;
    RoomIdentity.CompleteRoomId = RoomId;
    RoomIdentity.Type =
      URedwoodClientChatSubsystem::ParseRoomType(RoomTypeString);
    RoomIdentity.RedwoodId = RoomIdString;
    // TODO: RoomIdentity.Name

    OnJoinPrivateRoom.Broadcast(RoomIdentity);
  } else {
    UE_LOG(
      LogRedwoodChat, Error, TEXT("Failed to join private room: %s"), *Error
    );
  }
}

void URedwoodClientChatSubsystem::HandleRoomChatReceived(
  const TSharedRef<IXmppConnection> &Connection,
  const FString &RoomId,
  const FXmppUserJid &InUserJid,
  const TSharedRef<FXmppChatMessage> &ChatMsg
) {
  FString RoomTypeString;
  FString RoomIdString;
  RoomId.Split(TEXT("|"), &RoomTypeString, &RoomIdString);

  FString Body = ChatMsg->Body;
  if (Body.IsEmpty()) {
    return;
  }

  TSharedPtr<FJsonObject> MessageObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
  if (!FJsonSerializer::Deserialize(Reader, MessageObject)) {
    return;
  }

  FString SenderPlayerId;
  if (!MessageObject->TryGetStringField(TEXT("playerId"), SenderPlayerId) || SenderPlayerId.IsEmpty()) {
    return;
  }

  FString MessageText;
  if (!MessageObject->TryGetStringField(TEXT("message"), MessageText) || MessageText.IsEmpty()) {
    return;
  }

  bool bHasLocation = false;
  FVector Location;
  FString LocationString;
  if (MessageObject->TryGetStringField(TEXT("location"), LocationString) && !LocationString.IsEmpty()) {
    if (Location.InitFromString(LocationString)) {
      bHasLocation = true;
    }
  }

  FRedwoodChatRoomIdentity RoomIdentity;
  RoomIdentity.CompleteRoomId = bHasLocation ? TEXT("Nearby") : RoomId;
  RoomIdentity.Type = bHasLocation
    ? ERedwoodChatRoomType::Nearby
    : URedwoodClientChatSubsystem::ParseRoomType(RoomTypeString);
  RoomIdentity.RedwoodId = RoomIdString;
  // TODO: RoomIdentity.Name

  FRedwoodChatIdentity SenderIdentity;
  SenderIdentity.PlayerId = SenderPlayerId;
  SenderIdentity.Nickname = InUserJid.Resource;

  OnRoomChatReceived.Broadcast(
    RoomIdentity, SenderIdentity, ChatMsg->Timestamp, MessageText, Location
  );
}

void URedwoodClientChatSubsystem::JoinRoom(
  ERedwoodChatRoomType Type, FString Id
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  if (Type == ERedwoodChatRoomType::Custom) {
    UE_LOG(
      LogRedwoodChat,
      Error,
      TEXT("Use URedwoodClientChatSubsystem::JoinCustomRoom when Type == Custom"
      )
    );
    return;
  }

  bool bCharacterRoom =
    (bGuildsScopedToRealm && Type == ERedwoodChatRoomType::Guild) ||
    Type == ERedwoodChatRoomType::Party ||
    Type == ERedwoodChatRoomType::Realm ||
    Type == ERedwoodChatRoomType::Proxy ||
    Type == ERedwoodChatRoomType::Shard || Type == ERedwoodChatRoomType::Team ||
    Type == ERedwoodChatRoomType::Nearby;

  FString RoomTypeString = URedwoodClientChatSubsystem::SerializeRoomType(Type);
  FString RoomId = FString::Printf(TEXT("%s|%s"), *RoomTypeString, *Id);
  (bCharacterRoom ? XmppCharacterConnection : XmppPlayerConnection)
    ->MultiUserChat()
    ->JoinPrivateRoom(RoomId, Nickname, FString());
}

void URedwoodClientChatSubsystem::JoinCustomRoom(
  FString Id, FString Password, bool bJoinAsCharacter
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  CustomRoomUsesCharacter.Add(Id, bJoinAsCharacter);

  FString RoomTypeString =
    URedwoodClientChatSubsystem::SerializeRoomType(ERedwoodChatRoomType::Custom
    );
  FString RoomId = FString::Printf(TEXT("%s|%s"), *RoomTypeString, *Id);
  (bJoinAsCharacter ? XmppCharacterConnection : XmppPlayerConnection)
    ->MultiUserChat()
    ->JoinPrivateRoom(
      RoomId, bJoinAsCharacter ? CharacterName : Nickname, Password
    );
}

void URedwoodClientChatSubsystem::LeaveRoom(
  ERedwoodChatRoomType Type, FString Id
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  bool bCharacterRoom =
    (bGuildsScopedToRealm && Type == ERedwoodChatRoomType::Guild) ||
    Type == ERedwoodChatRoomType::Party ||
    Type == ERedwoodChatRoomType::Realm ||
    Type == ERedwoodChatRoomType::Proxy ||
    Type == ERedwoodChatRoomType::Shard || Type == ERedwoodChatRoomType::Team ||
    Type == ERedwoodChatRoomType::Nearby;

  if (Type == ERedwoodChatRoomType::Custom) {
    bool *bCustomRoomUsesCharacter = CustomRoomUsesCharacter.Find(Id);
    if (bCustomRoomUsesCharacter != nullptr) {
      bCharacterRoom = *bCustomRoomUsesCharacter;
    }
  }

  FString RoomTypeString = URedwoodClientChatSubsystem::SerializeRoomType(Type);
  FString RoomId = FString::Printf(TEXT("%s|%s"), *RoomTypeString, *Id);
  (bCharacterRoom ? XmppCharacterConnection : XmppPlayerConnection)
    ->MultiUserChat()
    ->ExitRoom(RoomId);
}

void URedwoodClientChatSubsystem::SendMessageToRoom(
  ERedwoodChatRoomType Type, FString Id, const FString &Message
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  bool bCharacterRoom =
    (bGuildsScopedToRealm && Type == ERedwoodChatRoomType::Guild) ||
    Type == ERedwoodChatRoomType::Party ||
    Type == ERedwoodChatRoomType::Realm ||
    Type == ERedwoodChatRoomType::Proxy ||
    Type == ERedwoodChatRoomType::Shard || Type == ERedwoodChatRoomType::Team ||
    Type == ERedwoodChatRoomType::Nearby;

  if (Type == ERedwoodChatRoomType::Custom) {
    bool *bCustomRoomUsesCharacter = CustomRoomUsesCharacter.Find(Id);
    if (bCustomRoomUsesCharacter != nullptr) {
      bCharacterRoom = *bCustomRoomUsesCharacter;
    }
  }

  TSharedPtr<FJsonObject> MessagePayload = MakeShareable(new FJsonObject);
  MessagePayload->SetStringField(
    TEXT("playerId"), bCharacterRoom ? CharacterId : PlayerId
  );
  MessagePayload->SetStringField(TEXT("message"), Message);

  FString JsonString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
  FJsonSerializer::Serialize(MessagePayload.ToSharedRef(), Writer);
  Writer->Close();

  FString RoomTypeString = URedwoodClientChatSubsystem::SerializeRoomType(Type);
  FString RoomId = FString::Printf(TEXT("%s|%s"), *RoomTypeString, *Id);
  (bCharacterRoom ? XmppCharacterConnection : XmppPlayerConnection)
    ->MultiUserChat()
    ->SendChat(RoomId, JsonString, FString());
}

void URedwoodClientChatSubsystem::SendNearbyMessage(
  const FString &ShardId, const FString &Message, const FVector &Location
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  TSharedPtr<FJsonObject> MessagePayload = MakeShareable(new FJsonObject);
  MessagePayload->SetStringField(TEXT("playerId"), CharacterId);
  MessagePayload->SetStringField(TEXT("message"), Message);
  MessagePayload->SetStringField(TEXT("location"), Location.ToString());

  FString JsonString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
  FJsonSerializer::Serialize(MessagePayload.ToSharedRef(), Writer);
  Writer->Close();

  FString RoomId = FString::Printf(TEXT("shard|%s"), *ShardId);
  XmppCharacterConnection->MultiUserChat()->SendChat(
    RoomId, JsonString, FString()
  );
}

void URedwoodClientChatSubsystem::SendMessageToPlayer(
  const FString &TargetPlayerId, const FString &Message
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  FXmppUserJid RecipientJid;
  RecipientJid.Id = TargetPlayerId;
  RecipientJid.Domain = XmppPlayerConnection->GetServer().Domain;

  TSharedPtr<FJsonObject> MessagePayload = MakeShareable(new FJsonObject);
  MessagePayload->SetStringField(TEXT("nickname"), Nickname);
  MessagePayload->SetStringField(TEXT("message"), Message);

  FString JsonString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
  FJsonSerializer::Serialize(MessagePayload.ToSharedRef(), Writer);
  Writer->Close();

  XmppPlayerConnection->PrivateChat()->SendChat(RecipientJid, JsonString);
}

void URedwoodClientChatSubsystem::SendMessageToCharacter(
  const FString &TargetCharacterId, const FString &Message
) {
  if (!IsConnected()) {
    // TODO log error
    return;
  }

  FXmppUserJid RecipientJid;
  RecipientJid.Id = TargetCharacterId;
  RecipientJid.Domain = XmppCharacterConnection->GetServer().Domain;

  TSharedPtr<FJsonObject> MessagePayload = MakeShareable(new FJsonObject);
  MessagePayload->SetStringField(TEXT("nickname"), CharacterName);
  MessagePayload->SetStringField(TEXT("message"), Message);

  FString JsonString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
  FJsonSerializer::Serialize(MessagePayload.ToSharedRef(), Writer);
  Writer->Close();

  XmppCharacterConnection->PrivateChat()->SendChat(RecipientJid, JsonString);
}

void URedwoodClientChatSubsystem::CreateCustomRoom(
  FString Id, FString Password, FRedwoodErrorOutputDelegate OnOutput
) {
  URedwoodClientGameSubsystem *GameSubsystem =
    GetGameInstance()->GetSubsystem<URedwoodClientGameSubsystem>();

  if (GameSubsystem) {
    URedwoodClientInterface *ClientInterface =
      GameSubsystem->GetClientInterface();

    if (ClientInterface) {
      if (!ClientInterface->IsDirectorConnected()) {
        OnOutput.ExecuteIfBound(TEXT("Not connected to the Director."));
        return;
      }

      // FORK(hollowed-oath): see InitializeChatConnection.
      OnOutput.ExecuteIfBound(TEXT("Text chat is not available."));
    } else {
      OnOutput.ExecuteIfBound(TEXT("Redwood Client Interface not found."));
    }
  } else {
    OnOutput.ExecuteIfBound(TEXT("Redwood Client Game Subsystem not found."));
  }
}