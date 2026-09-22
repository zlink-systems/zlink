#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ZLinkStreamConnector.h"

#include "EngineLobbyClientActor.generated.h"

UCLASS ()
class ENGINELOBBY_API AZLinkClientActor final : public AActor
{
    GENERATED_BODY ()

  public:
    AZLinkClientActor ();
    void Tick (float DeltaSeconds) override;

  protected:
    void BeginPlay () override;
    void EndPlay (const EEndPlayReason::Type EndPlayReason) override;

  private:
    void SendPing ();
    void SendJoin ();
    void SendChat ();
    void HandleResponse (const FZLinkStreamPacket &Packet);
    void HandlePacket (const FZLinkStreamPacket &Packet);
    void SetStatus (const FString &Status, FColor Color = FColor::White);

    UPROPERTY (EditAnywhere, Category = "Engine Lobby")
    FString Endpoint = TEXT ("ws://127.0.0.1:22700");

    UPROPERTY (EditAnywhere, Category = "Engine Lobby")
    FString PlayerName = TEXT ("unreal-player");

    UPROPERTY (EditAnywhere, Category = "Engine Lobby")
    FString FirstChat = TEXT ("hello from Unreal");

    UPROPERTY ()
    TObjectPtr<UZLinkStreamConnector> Connector;

    FDelegateHandle PacketHandle;
    FDelegateHandle ResponseHandle;
};
