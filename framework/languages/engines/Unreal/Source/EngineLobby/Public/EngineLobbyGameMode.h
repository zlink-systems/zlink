#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "EngineLobbyGameMode.generated.h"

class AZLinkClientActor;

UCLASS ()
class ENGINELOBBY_API AEngineLobbyGameMode final : public AGameModeBase
{
    GENERATED_BODY ()

  protected:
    void StartPlay () override;

  private:
    UPROPERTY ()
    TObjectPtr<AZLinkClientActor> Client;
};
