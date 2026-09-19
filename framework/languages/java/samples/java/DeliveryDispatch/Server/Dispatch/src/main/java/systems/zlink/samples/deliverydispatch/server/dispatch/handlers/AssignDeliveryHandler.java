package systems.zlink.samples.deliverydispatch.server.dispatch.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.dispatch.DispatchWorkQueue;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

/**
 * The one-way admission of a new delivery onto the dispatch channel (common sample spec section
 * 7.1). The HTTP edge never calls into worker state directly — it sends this message, and this
 * handler is the only path that hands it to the in-process work queue.
 */
@ZLinkHandlerGroup(SampleNames.DispatchChannel)
public final class AssignDeliveryHandler
    implements ZLinkSendHandler<Messages.AssignDeliveryMsg> {
    private final DispatchWorkQueue queue;

    public AssignDeliveryHandler(DispatchWorkQueue queue) {
        this.queue = queue;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.AssignDeliveryMsg message,
        ZLinkMessageContext context) {
        queue.enqueue(message);
        System.out.println("deliverydispatch dispatch-channel: enqueued delivery="
            + message.deliveryId() + " customer=" + message.customerId());
        return CompletableFuture.completedFuture(null);
    }
}
