#include "EngineLobbyGameMode.h"

#include "EngineLobbyClientActor.h"
#include "Engine/World.h"

void AEngineLobbyGameMode::StartPlay ()
{
    Super::StartPlay ();
    Client = GetWorld ()->SpawnActor<AZLinkClientActor> ();
}
