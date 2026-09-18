package systems.zlink.tutorial.server.actors;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.tutorial.server.spots.LobbySpot;
import systems.zlink.tutorial.shared.Contracts;

// A message addressed to a player runs inside the Spot the player currently
// occupies, so a handler receives both the Spot and the player.

// --8<-- [start:actor-send-handler]
public final class ChangeNicknameHandler
    implements ZLinkEntrySpotActorSendHandler<LobbySpot, Player, Contracts.ChangeNickname> {

    @Override
    public CompletionStage<Void> handle(
        LobbySpot lobby,
        Player player,
        ZLinkMessageContext context,
        Contracts.ChangeNickname message) {
        player.rename(message.nickname());

        // --8<-- [start:actor-push]
        // Reaches the connection bound to this player. The same handler also runs
        // for a player nobody is connected to -- the HTTP path of the Actor step --
        // and on this binding that push fails instead of doing nothing, so the
        // failure is dropped rather than failing the rename.
        try {
            return player.context().boundSession()
                .send(
                    new Contracts.NicknameChanged(
                        player.nickname()))
                .submit()
                .exceptionally(error -> null);
        } catch (RuntimeException noConnection) {
            return CompletableFuture
                .completedFuture(null);
        }
        // --8<-- [end:actor-push]
    }
}
// --8<-- [end:actor-send-handler]
