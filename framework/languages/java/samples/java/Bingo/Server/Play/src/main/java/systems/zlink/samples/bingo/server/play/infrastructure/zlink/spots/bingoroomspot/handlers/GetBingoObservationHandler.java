package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;
import systems.zlink.samples.bingo.shared.contracts.Messages;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class GetBingoObservationHandler
        implements ZLinkSpotRequestHandler<
                BingoRoomSpot, Messages.GetBingoObservationReq, Messages.GetBingoObservationRes> {
    @Override
    public CompletionStage<Messages.GetBingoObservationRes> handle(
            BingoRoomSpot spot, Messages.GetBingoObservationReq request) {
        return CompletableFuture.completedFuture(spot.observation());
    }
}
