package systems.zlink.samples.deliverydispatch.server.dispatch.handlers;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.dispatch.DeliveryOfferStore;
import systems.zlink.samples.deliverydispatch.server.dispatch.DeliveryOfferStore.DeliveryOffer;
import systems.zlink.samples.deliverydispatch.server.dispatch.DispatchWorker;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The courier's decision, arriving as its own inbound message rather than as the reply to a request
 * nobody could have made. A decision naming an attempt other than the one on record came back after
 * the offer had already been reassigned, and is dropped — that check is what makes it safe never to
 * wait (common sample spec section 7.4).
 */
@ZLinkHandlerGroup(SampleNames.DispatchChannel)
public final class OfferDeliveryResultHandler
    implements ZLinkSendHandler<Messages.OfferDeliveryResultMsg> {
    private final DeliveryOfferStore offers;
    private final DispatchWorker worker;

    public OfferDeliveryResultHandler(DeliveryOfferStore offers, DispatchWorker worker) {
        this.offers = offers;
        this.worker = worker;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.OfferDeliveryResultMsg message,
        ZLinkMessageContext context) {
        Optional<DeliveryOffer> offer = offers.settle(message.deliveryId(), message.attempt());
        if (offer.isEmpty()) {
            System.out.println("deliverydispatch dispatch: dropped a late decision delivery="
                + message.deliveryId() + " attempt=" + message.attempt());
            return CompletableFuture.completedFuture(null);
        }

        System.out.println("deliverydispatch dispatch: decision delivery=" + message.deliveryId()
            + " courier=" + message.courierId() + " attempt=" + message.attempt()
            + " accepted=" + message.accepted());
        return worker.settle(offer.get(), message.accepted(), message.reason());
    }
}
