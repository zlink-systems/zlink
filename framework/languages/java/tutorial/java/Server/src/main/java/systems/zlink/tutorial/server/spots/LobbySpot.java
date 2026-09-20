package systems.zlink.tutorial.server.spots;

import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.tutorial.server.actors.ChangeNicknameHandler;
import systems.zlink.tutorial.server.actors.GetPlayerHandler;
import systems.zlink.tutorial.server.actors.Player;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:entry-spot]
// Every new player lands here before joining a room, and returns here after
// leaving one. A node that hosts players registers exactly one of these.
public final class LobbySpot implements ZLinkEntrySpot<Player> {

    private final ZLinkEntrySpotContext context;

    public LobbySpot(ZLinkEntrySpotContext context) {
        this.context = context;
        // --8<-- [start:actor-handler-register]
        // A message addressed to a player runs inside the Spot the player
        // currently occupies, so these handlers are registered on the lobby.
        context.handlers().addHandler(ChangeNicknameHandler.class);
        context.handlers().addHandler(GetPlayerHandler.class);
        // --8<-- [end:actor-handler-register]
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onJoinedActor(Player player) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(Player player) {
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:entry-spot]
