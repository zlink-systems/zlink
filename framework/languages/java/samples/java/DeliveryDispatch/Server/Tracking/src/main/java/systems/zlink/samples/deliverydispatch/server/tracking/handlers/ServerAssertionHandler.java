package systems.zlink.samples.deliverydispatch.server.tracking.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.deliverydispatch.server.configuration.EvidenceStore;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup("tracking")
public final class ServerAssertionHandler
    implements ZLinkRequestHandler<Messages.ServerAssertionReq, Messages.ServerAssertionRes> {
    private final EvidenceStore evidenceStore;

    public ServerAssertionHandler(EvidenceStore evidenceStore) {
        this.evidenceStore = evidenceStore;
    }

    @Override
    public CompletionStage<Messages.ServerAssertionRes> handle(
        Messages.ServerAssertionReq request,
        ZLinkMessageContext context) {
        return CompletableFuture.completedFuture(evidenceStore.assertSequences(
            request.successfulDeliveryId(),
            request.reassignedDeliveryId()));
    }
}
