package systems.zlink.samples.deliverydispatch.server.dispatch;

import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTimings;
import systems.zlink.samples.deliverydispatch.server.dispatch.DeliveryOfferStore.DeliveryOffer;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The dispatch flow. No step of it waits for a courier: the offer goes out one-way, the turn ends,
 * and the offer row decides what happens next — either a decision arrives, or the deadline passes
 * and the sweeper reassigns (common sample spec section 7.4).
 */
public final class DispatchWorker {
    /** Who gets offered a delivery, and in what order. The worker's policy, not the node's. */
    private static final List<String> Candidates = List.of("courier-a", "courier-b");

    private final ZLinkClient channels;
    private final ZLinkActorClient actors;
    private final DeliveryOfferStore offers;

    public DispatchWorker(
        ZLinkClient channels,
        ZLinkActorClient actors,
        DeliveryOfferStore offers) {
        this.channels = channels;
        this.actors = actors;
        this.offers = offers;
    }

    /** The first offer. Records it, sends it, and returns — nobody is left waiting. */
    public CompletionStage<Void> dispatch(Messages.AssignDeliveryMsg request) {
        String courierId = Candidates.get(0);
        int attempt = offers.offer(request, 0, SampleTimings.CourierDecisionTimeout);
        return publishStatus(request, Messages.DeliveryStatus.Assigned, courierId)
            .thenCompose(ignored -> offer(request, courierId, attempt));
    }

    /** A decision arrived. Accepted carries the delivery through; refused reassigns. */
    public CompletionStage<Void> settle(DeliveryOffer offer, boolean accepted, String reason) {
        String courierId = Candidates.get(offer.candidateIndex());
        if (!accepted) {
            System.out.println("deliverydispatch dispatch: courier=" + courierId
                + " did not take delivery=" + offer.request().deliveryId()
                + " (" + (reason == null ? "refused" : reason) + ")");
            return reassign(offer);
        }

        return publishStatus(offer.request(), Messages.DeliveryStatus.Accepted, courierId)
            .thenCompose(ignored -> publishStatus(
                offer.request(), Messages.DeliveryStatus.PickedUp, courierId))
            .thenCompose(ignored -> publishStatus(
                offer.request(), Messages.DeliveryStatus.Delivered, courierId))
            .thenAccept(ignored -> offers.close(offer.request().deliveryId()));
    }

    /**
     * The offer lapsed, or the courier refused. Either way the next candidate gets it. The deadline
     * lives here rather than on the courier node: a node that timed the offer and manufactured a
     * refusal would be hiding the dispatch policy (common sample spec section 7.4).
     */
    public CompletionStage<Void> reassign(DeliveryOffer offer) {
        int nextIndex = offer.candidateIndex() + 1;
        if (nextIndex >= Candidates.size()) {
            System.err.println("deliverydispatch dispatch: delivery=" + offer.request().deliveryId()
                + " was rejected by all couriers");
            return publishStatus(
                    offer.request(),
                    Messages.DeliveryStatus.Failed,
                    Candidates.get(Candidates.size() - 1))
                .thenAccept(ignored -> offers.close(offer.request().deliveryId()));
        }

        String courierId = Candidates.get(nextIndex);
        int attempt = offers.offer(offer.request(), nextIndex, SampleTimings.CourierDecisionTimeout);
        return publishStatus(offer.request(), Messages.DeliveryStatus.Reassigned, courierId)
            .thenCompose(ignored -> offer(offer.request(), courierId, attempt));
    }

    public CompletionStage<Messages.ServerAssertionResponse> assertServerEvidence(
        Messages.ServerAssertionRequest request) {
        return channels
            .requestToChannel(SampleNames.TrackingChannel, request)
            .submit(Messages.ServerAssertionResponse.class);
    }

    /** The offer is a one-way send: the turn that sends it ends right there. */
    private CompletionStage<Void> offer(
        Messages.AssignDeliveryMsg request,
        String courierId,
        int attempt) {
        return actors
            .sendToActor(
                courierId,
                new Messages.OfferDeliveryMsg(
                    courierId,
                    request.deliveryId(),
                    attempt,
                    request.pickupAddress(),
                    request.dropoffAddress()))
            .submit();
    }

    private CompletionStage<Void> publishStatus(
        Messages.AssignDeliveryMsg request,
        Messages.DeliveryStatus status,
        String courierId) {
        return channels.requestToChannel(
                SampleNames.TrackingChannel,
                new Messages.DeliveryStatusChangedReq(
                    request.deliveryId(),
                    status,
                    courierId,
                    Instant.now().toString()))
            .submit(Messages.DeliveryStatusChangedRes.class)
            .thenAccept(ignored -> {
            });
    }
}
