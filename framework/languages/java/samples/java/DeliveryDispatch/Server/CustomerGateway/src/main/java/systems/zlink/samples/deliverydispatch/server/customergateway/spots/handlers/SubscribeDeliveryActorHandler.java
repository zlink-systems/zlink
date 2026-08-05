package systems.zlink.samples.deliverydispatch.server.customergateway.spots.handlers;

import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.deliverydispatch.server.customergateway.CustomerActor;
import systems.zlink.samples.deliverydispatch.server.customergateway.spots.CustomerEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class SubscribeDeliveryActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        CustomerEntrySpot,
        CustomerActor,
        Messages.SubscribeDeliveryReq,
        Messages.SubscribeDeliveryRes> {
    @Override
    public CompletionStage<Messages.SubscribeDeliveryRes> handle(
        CustomerEntrySpot entrySpot,
        CustomerActor actor,
        ZLinkMessageContext context,
        Messages.SubscribeDeliveryReq request) {
        return CompletableFuture.completedFuture(entrySpot.subscribe(actor, request));
    }
}
