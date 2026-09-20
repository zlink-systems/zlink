package systems.zlink.tutorial.server.spots;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.tutorial.shared.Contracts;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// Spot handlers live in their own classes and take the target room as the first
// argument. The two type arguments are what the Framework matches a packet
// against, so no packet name is written anywhere.
public final class PostChatHandler implements ZLinkSpotPacketHandler<GameRoom, Contracts.PostChat> {

    @Override
    public CompletionStage<Void> handle(GameRoom room, Contracts.PostChat message) {
        room.append(message.playerId() + ": " + message.text());
        return CompletableFuture.completedFuture(null);
    }
}
