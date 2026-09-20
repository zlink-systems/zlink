package systems.zlink.tutorial.server.actors;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.tutorial.server.spots.LobbySpot;
import systems.zlink.tutorial.shared.Contracts;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:actor-request-handler]
// The return value is the reply. This handler only reads.
public final class GetPlayerHandler
        implements ZLinkEntrySpotActorRequestHandler<
                LobbySpot, Player, Contracts.GetPlayer, Contracts.PlayerInfo> {

    @Override
    public CompletionStage<Contracts.PlayerInfo> handle(
            LobbySpot lobby,
            Player player,
            ZLinkMessageContext context,
            Contracts.GetPlayer request) {
        return CompletableFuture.completedFuture(
                new Contracts.PlayerInfo(player.context().actorId(), player.nickname()));
    }
}
// --8<-- [end:actor-request-handler]
