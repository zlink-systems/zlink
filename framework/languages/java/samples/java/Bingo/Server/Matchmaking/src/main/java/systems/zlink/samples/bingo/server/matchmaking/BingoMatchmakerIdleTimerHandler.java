package systems.zlink.samples.bingo.server.matchmaking;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class BingoMatchmakerIdleTimerHandler {
    public CompletionStage<Void> handle(
        BingoMatchmaker spot,
        ZLinkTimerTick tick) {
        spot.closeIfIdle();
        return CompletableFuture.completedFuture(null);
    }
}
