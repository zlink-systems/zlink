package systems.zlink.samples.bingo.server.matchmaking;

import systems.zlink.framework.spots.ZLinkTimerTick;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class BingoMatchmakerIdleTimerHandler {
    public CompletionStage<Void> handle(BingoMatchmaker spot, ZLinkTimerTick tick) {
        spot.closeIfIdle();
        return CompletableFuture.completedFuture(null);
    }
}
