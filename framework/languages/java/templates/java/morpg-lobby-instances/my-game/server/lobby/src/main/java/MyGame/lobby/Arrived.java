package MyGame.lobby;
import MyGame.*;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import java.util.concurrent.*;
public final class Arrived implements ZLinkSpotPacketHandler<Party, DungeonMemberEntered> {
    public CompletionStage<Void> handle(Party spot, DungeonMemberEntered r) {
        String reason = spot.state.arrived(r.runId(), r.playerId()); if (reason != null) throw new IllegalArgumentException(reason);
        return CompletableFuture.completedFuture(null);
    }
}
