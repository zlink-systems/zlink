#include "EngineLobbyClientActor.h"

#include "EngineLobbyProtocol.h"
#include "Engine/Engine.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FName PacketName (const char *Name)
{
    return FName (UTF8_TO_TCHAR (Name));
}

FString JsonField (const char *Name)
{
    return FString (UTF8_TO_TCHAR (Name));
}

FString EncodeStringField (const char *Field, const FString &Value)
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject> ();
    Json->SetStringField (JsonField (Field), Value);
    FString Payload;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create (&Payload);
    FJsonSerializer::Serialize (Json, Writer);
    return Payload;
}

TSharedPtr<FJsonObject> DecodePayload (const FZLinkStreamPacket &Packet)
{
    if (Packet.Payload.IsEmpty ()) {
        return nullptr;
    }
    const FUTF8ToTCHAR Converted (reinterpret_cast<const ANSICHAR *> (Packet.Payload.GetData ()),
                                  Packet.Payload.Num ());
    const FString Payload (Converted.Length (), Converted.Get ());
    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create (Payload);
    return FJsonSerializer::Deserialize (Reader, Json) ? Json : nullptr;
}

bool ReadRequiredString (const TSharedPtr<FJsonObject> &Json, const char *Field, FString &Value)
{
    return Json.IsValid () && Json->TryGetStringField (JsonField (Field), Value)
           && !Value.IsEmpty ();
}
} // namespace

AZLinkClientActor::AZLinkClientActor ()
{
    PrimaryActorTick.bCanEverTick = true;
}

// --8<-- [start:connect]
void AZLinkClientActor::BeginPlay ()
{
    Super::BeginPlay ();
    Connector = NewObject<UZLinkStreamConnector> (this);
    ResponseHandle =
      Connector->OnRequestCompletedNative.AddUObject (this, &AZLinkClientActor::HandleResponse);
    PacketHandle =
      Connector->OnPacketReceivedNative.AddUObject (this, &AZLinkClientActor::HandlePacket);

    SetStatus (TEXT ("Engine Lobby: connecting"));
    Connector->Connect (Endpoint);
    if (!Connector->IsConnected ()) {
        SetStatus (TEXT ("Engine Lobby: connection failed"), FColor::Red);
        return;
    }
    SendPing ();
}

void AZLinkClientActor::SendPing ()
{
    Connector->RequestJson (PacketName (engine_lobby::packet::ping_req),
                            EncodeStringField (engine_lobby::field::sent_at_unix_ms, TEXT ("1000")),
                            5.0F);
}
// --8<-- [end:connect]

// --8<-- [start:pump]
void AZLinkClientActor::Tick (float DeltaSeconds)
{
    Super::Tick (DeltaSeconds);
    if (Connector != nullptr) {
        Connector->Tick (DeltaSeconds);
    }
}
// --8<-- [end:pump]

// --8<-- [start:handler]
void AZLinkClientActor::HandleResponse (const FZLinkStreamPacket &Packet)
{
    const TSharedPtr<FJsonObject> Json = DecodePayload (Packet);
    if (Packet.PacketName == PacketName (engine_lobby::packet::ping_res)) {
        FString SentAt;
        if (!ReadRequiredString (Json, engine_lobby::field::sent_at_unix_ms, SentAt)
            || SentAt != TEXT ("1000")) {
            SetStatus (TEXT ("Engine Lobby: invalid PingRes"), FColor::Red);
            return;
        }
        SendJoin ();
        return;
    }

    if (Packet.PacketName == PacketName (engine_lobby::packet::join_res)) {
        FString ActorId;
        FString Name;
        if (!ReadRequiredString (Json, engine_lobby::field::actor_id, ActorId)
            || !ReadRequiredString (Json, engine_lobby::field::name, Name) || Name != PlayerName) {
            SetStatus (TEXT ("Engine Lobby: invalid JoinRes"), FColor::Red);
            return;
        }
        SetStatus (FString::Printf (TEXT ("joined as %s (%s)"), *Name, *ActorId), FColor::Green);
        SendChat ();
    }
}

void AZLinkClientActor::HandlePacket (const FZLinkStreamPacket &Packet)
{
    if (Packet.PacketName != PacketName (engine_lobby::packet::chat_notify)) {
        return;
    }

    const TSharedPtr<FJsonObject> Json = DecodePayload (Packet);
    FString ActorId;
    FString Name;
    FString Text;
    if (!ReadRequiredString (Json, engine_lobby::field::actor_id, ActorId)
        || !ReadRequiredString (Json, engine_lobby::field::name, Name)
        || !ReadRequiredString (Json, engine_lobby::field::text, Text)) {
        SetStatus (TEXT ("Engine Lobby: invalid ChatNotify"), FColor::Red);
        return;
    }
    SetStatus (FString::Printf (TEXT ("%s: %s"), *Name, *Text), FColor::Green);
}

void AZLinkClientActor::SendJoin ()
{
    Connector->RequestJson (PacketName (engine_lobby::packet::join_req),
                            EncodeStringField (engine_lobby::field::name, PlayerName), 5.0F);
}

void AZLinkClientActor::SendChat ()
{
    Connector->SendJson (PacketName (engine_lobby::packet::chat_msg),
                         EncodeStringField (engine_lobby::field::text, FirstChat));
}
// --8<-- [end:handler]

// --8<-- [start:lifecycle]
void AZLinkClientActor::EndPlay (const EEndPlayReason::Type EndPlayReason)
{
    if (Connector != nullptr) {
        Connector->OnPacketReceivedNative.Remove (PacketHandle);
        Connector->OnRequestCompletedNative.Remove (ResponseHandle);
        Connector->ShutdownForMapUnload ();
    }
    Super::EndPlay (EndPlayReason);
}
// --8<-- [end:lifecycle]

void AZLinkClientActor::SetStatus (const FString &Status, FColor Color)
{
    UE_LOG (LogTemp, Display, TEXT ("%s"), *Status);
    if (GEngine != nullptr) {
        GEngine->AddOnScreenDebugMessage (1, 10.0F, Color, Status);
    }
}
