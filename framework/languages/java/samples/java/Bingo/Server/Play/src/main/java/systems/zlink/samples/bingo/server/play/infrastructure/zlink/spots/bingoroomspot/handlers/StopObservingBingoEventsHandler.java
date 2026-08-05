package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class StopObservingBingoEventsHandler
    implements ZLinkSpotActorRequestHandler<
        BingoRoomSpot,
        PlayerActor,
        Messages.StopObservingBingoEventsReq,
        Messages.StopObservingBingoEventsRes> {
    @Override
    public java.util.concurrent.CompletionStage<Messages.StopObservingBingoEventsRes> handle(
        BingoRoomSpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.StopObservingBingoEventsReq request) {
        return java.util.concurrent.CompletableFuture.completedFuture(spot.stopObserving(actor, request));
    }
}
