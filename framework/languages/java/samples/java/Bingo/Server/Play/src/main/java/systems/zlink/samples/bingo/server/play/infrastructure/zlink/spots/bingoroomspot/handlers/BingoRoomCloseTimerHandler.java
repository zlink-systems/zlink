package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class BingoRoomCloseTimerHandler implements ZLinkSpotTimerHandler<BingoRoomSpot> {
    private static final Logger logger = LoggerFactory.getLogger(BingoRoomCloseTimerHandler.class);

    @Override
    public CompletionStage<Void> handle(BingoRoomSpot spot, ZLinkTimerTick tick) {
        if (!spot.tryRequestClose()) return CompletableFuture.completedFuture(null);
        logger.info("bingo-lifecycle room-close spot={}", spot.context().spotId());
        spot.context().close();
        return CompletableFuture.completedFuture(null);
    }
}
