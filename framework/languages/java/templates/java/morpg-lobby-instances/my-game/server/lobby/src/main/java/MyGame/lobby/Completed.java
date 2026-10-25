package MyGame.lobby;
import MyGame.*;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import java.util.concurrent.*;
public final class Completed implements ZLinkSpotPacketHandler<Party, RunCompleted> {
    public CompletionStage<Void> handle(Party spot, RunCompleted r) {
        String reason = spot.state.completed(r.runId()); if (reason != null) throw new IllegalArgumentException(reason);
        return CompletableFuture.completedFuture(null);
    }
}
