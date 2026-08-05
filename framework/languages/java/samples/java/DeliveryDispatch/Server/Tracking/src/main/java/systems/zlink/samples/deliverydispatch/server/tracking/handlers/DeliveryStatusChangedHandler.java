package systems.zlink.samples.deliverydispatch.server.tracking.handlers;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.deliverydispatch.server.configuration.EvidenceStore;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup("tracking")
public final class DeliveryStatusChangedHandler
    implements ZLinkRequestHandler<Messages.DeliveryStatusChangedReq, Messages.DeliveryStatusChangedRes> {
    private static final String SAMPLE_CUSTOMER_ID = "customer-1";
    private final EvidenceStore evidenceStore;
    private final ZLinkActorClient actors;
    private final ZLinkActorDirectory actorRefs;

    public DeliveryStatusChangedHandler(
        EvidenceStore evidenceStore,
        ZLinkActorClient actors,
        ZLinkActorDirectory actorRefs) {
        this.evidenceStore = evidenceStore;
        this.actors = actors;
        this.actorRefs = actorRefs;
    }

    @Override
    public CompletionStage<Messages.DeliveryStatusChangedRes> handle(
        Messages.DeliveryStatusChangedReq request,
        ZLinkMessageContext context) {
        evidenceStore.append(request);
        return actorRefs.find(SAMPLE_CUSTOMER_ID).thenCompose(found -> {
            var actor = found.orElseThrow(() -> new IllegalStateException(
                "customer actor not found: " + SAMPLE_CUSTOMER_ID));
            return actors.sendToActor(actor.actorId(), new Messages.DeliveryStatusUpdatedMsg(
                    request.deliveryId(), SAMPLE_CUSTOMER_ID, request.status(),
                    request.courierId(), request.occurredAt()))
                .submit()
                .thenApply(ignored ->
                    new Messages.DeliveryStatusChangedRes(
                        request.deliveryId(), request.status()));
        });
    }
}
