package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot.handlers;

import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class ObserveBingoEventsHandler
    implements ZLinkEntrySpotActorRequestHandler<
        BingoEntrySpot,
        PlayerActor,
        Messages.ObserveBingoEventsReq,
        Messages.ObserveBingoEventsRes> {
    @Override
    public java.util.concurrent.CompletionStage<Messages.ObserveBingoEventsRes> handle(
        BingoEntrySpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.ObserveBingoEventsReq request) {
        return spot.observeEvents(actor, request);
    }
}
