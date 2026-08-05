package systems.zlink.samples.deliverydispatch.server.courierspotnode;

import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * A courier. It pushes the offer to whoever holds this courier's stream session and its turn ends
 * there — it does not wait for the answer, and there is nothing left here that could wait (common
 * sample spec section 7.4). The answer arrives later as a {@code CourierDecision} from the session,
 * and the only thing the actor has to remember in between is which attempt the offer belonged to,
 * so the decision can be paired with it.
 */
public final class CourierActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private final Object gate = new Object();
    private final Map<String, Integer> offeredAttempts = new HashMap<>();

    public CourierActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    /** Pushes the offer and returns. The courier takes as long as it takes. */
    public CompletionStage<Void> offer(Messages.OfferDeliveryMsg offer) {
        synchronized (gate) {
            offeredAttempts.put(offer.deliveryId(), offer.attempt());
        }
        return context.boundSession()
            .send(new Messages.OfferDeliveryNotify(
                offer.courierId(),
                offer.deliveryId(),
                offer.pickupAddress(),
                offer.dropoffAddress()))
            .submit();
    }

    /**
     * The attempt the courier is answering, or empty when this actor knows of no such offer — it
     * was already answered, or this actor was never offered it.
     */
    public Optional<Integer> takeOfferedAttempt(String deliveryId) {
        synchronized (gate) {
            return Optional.ofNullable(offeredAttempts.remove(deliveryId));
        }
    }
}
