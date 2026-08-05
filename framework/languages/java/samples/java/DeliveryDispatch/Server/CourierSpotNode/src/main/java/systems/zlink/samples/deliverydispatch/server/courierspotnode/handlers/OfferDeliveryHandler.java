package systems.zlink.samples.deliverydispatch.server.courierspotnode.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The offer arrives as a one-way send, is handed to the courier actor, and this handler returns —
 * the spot's serial queue is given straight back, so it is not held for the length of a courier's
 * reaction time. The node does not time the offer either: that deadline belongs to the dispatch
 * worker (common sample spec section 7.4).
 */
public final class OfferDeliveryHandler
    implements ZLinkSpotPacketHandler<CourierEntrySpot, Messages.OfferDeliveryMsg> {
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;

    public OfferDeliveryHandler(ZLinkActorManager actors, ZLinkActorClient actorClient) {
        this.actors = actors;
        this.actorClient = actorClient;
    }

    @Override
    public CompletionStage<Void> handle(CourierEntrySpot spot, Messages.OfferDeliveryMsg message) {
        return actors.find(message.courierId()).thenCompose(found -> {
            var actorRef = found.orElseThrow(() -> new IllegalStateException(
                "Courier actor is not bound: " + message.courierId()));
            return actorClient.sendToActor(actorRef.actorId(), message).submit();
        });
    }
}
