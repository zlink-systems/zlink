package systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.handlers;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.CourierActor;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The courier's decision, sent back to dispatch one-way. The node neither judges it nor times it —
 * it carries the attempt the offer was made under, and dispatch decides whether that attempt is
 * still the current one (common sample spec section 7.4).
 */
public final class CourierDecisionActorHandler
    implements ZLinkEntrySpotActorSendHandler<
        CourierEntrySpot,
        CourierActor,
        Messages.CourierDecision> {
    private final ZLinkClient channels;

    public CourierDecisionActorHandler(ZLinkClient channels) {
        this.channels = channels;
    }

    @Override
    public CompletionStage<Void> handle(
        CourierEntrySpot entrySpot,
        CourierActor actor,
        ZLinkMessageContext context,
        Messages.CourierDecision message) {
        Optional<Integer> attempt = actor.takeOfferedAttempt(message.deliveryId());
        if (attempt.isEmpty()) {
            System.err.println("deliverydispatch courier-actor: decision for an unknown offer"
                + " delivery=" + message.deliveryId() + " courier=" + actor.actorId());
            return CompletableFuture.completedFuture(null);
        }

        return channels.sendToChannel(
                SampleNames.DispatchChannel,
                new Messages.OfferDeliveryResultMsg(
                    message.deliveryId(),
                    message.courierId(),
                    attempt.get(),
                    message.accepted(),
                    message.reason()))
            .submit()
            .thenRun(() -> System.out.println(
                "deliverydispatch courier-actor: decision delivery="
                    + message.deliveryId() + " courier=" + actor.actorId()
                    + " attempt=" + attempt.get() + " accepted=" + message.accepted()));
    }
}
