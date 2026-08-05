package systems.zlink.samples.deliverydispatch.server.dispatch;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The offers dispatch is waiting on. This is the whole of the waiting: no blocked thread and no
 * held serial queue anywhere, just rows with deadlines (common sample spec section 7.4).
 */
public final class DeliveryOfferStore {
    private final Object gate = new Object();
    private final Map<String, MutableOffer> offers = new HashMap<>();

    /** Records a new offer and returns its attempt number. */
    public int offer(Messages.AssignDeliveryMsg request, int candidateIndex, Duration timeout) {
        synchronized (gate) {
            MutableOffer offer = offers.computeIfAbsent(
                request.deliveryId(), ignored -> new MutableOffer(request));
            offer.request = request;
            offer.candidateIndex = candidateIndex;
            offer.attempt += 1;
            offer.deadline = Instant.now().plus(timeout);
            offer.settled = false;
            return offer.attempt;
        }
    }

    /**
     * Closes the offer a decision belongs to and returns it. A decision naming another attempt
     * arrived after the offer was reassigned, and an offer already settled has been answered —
     * both are dropped.
     */
    public Optional<DeliveryOffer> settle(String deliveryId, int attempt) {
        synchronized (gate) {
            MutableOffer offer = offers.get(deliveryId);
            if (offer == null || offer.settled || offer.attempt != attempt) {
                return Optional.empty();
            }
            offer.settled = true;
            return Optional.of(offer.snapshot());
        }
    }

    /** The offers whose deadline has passed. The sweeper reassigns them. */
    public List<DeliveryOffer> takeExpired() {
        Instant now = Instant.now();
        synchronized (gate) {
            List<DeliveryOffer> expired = new ArrayList<>();
            for (MutableOffer offer : offers.values()) {
                if (offer.settled || offer.deadline.isAfter(now)) {
                    continue;
                }
                offer.settled = true;
                expired.add(offer.snapshot());
            }
            return expired;
        }
    }

    public void close(String deliveryId) {
        synchronized (gate) {
            offers.remove(deliveryId);
        }
    }

    /** One delivery's offer in flight. The row decides what happens next. */
    public record DeliveryOffer(
        Messages.AssignDeliveryMsg request,
        int candidateIndex,
        int attempt,
        Instant deadline) {
    }

    private static final class MutableOffer {
        private Messages.AssignDeliveryMsg request;
        private int candidateIndex;
        private int attempt;
        private Instant deadline = Instant.EPOCH;
        private boolean settled;

        private MutableOffer(Messages.AssignDeliveryMsg request) {
            this.request = request;
        }

        private DeliveryOffer snapshot() {
            return new DeliveryOffer(request, candidateIndex, attempt, deadline);
        }
    }
}
