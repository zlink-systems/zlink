package systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.CourierActor;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The offer, in the courier actor's own turn: push it to the bound session and return (common
 * sample spec section 7.4).
 */
public final class OfferDeliveryActorHandler
    implements ZLinkEntrySpotActorSendHandler<
        CourierEntrySpot,
        CourierActor,
        Messages.OfferDeliveryMsg> {
    @Override
    public CompletionStage<Void> handle(
        CourierEntrySpot entrySpot,
        CourierActor actor,
        ZLinkMessageContext context,
        Messages.OfferDeliveryMsg message) {
        return actor.offer(message).thenRun(() ->
            System.out.println("deliverydispatch courier-actor: offer pushed delivery="
                + message.deliveryId() + " courier=" + actor.actorId()
                + " attempt=" + message.attempt()));
    }
}
