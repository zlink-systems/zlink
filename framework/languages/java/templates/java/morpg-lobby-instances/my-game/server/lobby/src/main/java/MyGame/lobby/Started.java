package MyGame.lobby;
import MyGame.*;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import java.util.concurrent.*;
public final class Started implements ZLinkSpotPacketHandler<Party, RunStarted> {
    public CompletionStage<Void> handle(Party spot, RunStarted r) {
        String reason = spot.state.started(r.runId()); if (reason != null) throw new IllegalArgumentException(reason);
        return CompletableFuture.completedFuture(null);
    }
}
