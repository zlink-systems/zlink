package systems.zlink.tutorial.server.actors;

import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
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
        // Pushes over the connection bound to this player. The same handler also runs
        // on an HTTP path with no bound connection, where push ends with InvalidOperation.
        // Rename is already complete, so only that failure is discarded.
        return player.context().boundSession()
            .send(
                new Contracts.NicknameChanged(
                    player.nickname()))
            .submit()
            .exceptionally(error -> {
                if (error instanceof ZLinkFrameworkException framework
                    && framework.kind() == ZLinkFrameworkErrorKind.INVALID_OPERATION) {
                    return null;
                }
                throw new CompletionException(error);
            });
        // --8<-- [end:actor-push]
    }
}
// --8<-- [end:actor-send-handler]
