package systems.zlink.samples.deliverydispatch.server.customergateway.spots;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.samples.deliverydispatch.server.customergateway.CustomerActor;
import systems.zlink.samples.deliverydispatch.server.customergateway.CustomerActorDirectory;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class CustomerEntrySpot implements ZLinkEntrySpot<CustomerActor> {
    private final ZLinkEntrySpotContext context;
    private final CustomerActorDirectory customers;

    public CustomerEntrySpot(
        ZLinkEntrySpotContext context,
        CustomerActorDirectory customers) {
        this.context = context;
        this.customers = customers;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        CustomerActor actor,
        ZLinkMessage createRequest) {
        customers.register(actor);
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(CustomerActor actor) {
        customers.register(actor);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(CustomerActor actor) {
        customers.remove(actor.actorId());
        return CompletableFuture.completedFuture(null);
    }

    public Messages.SubscribeDeliveryRes subscribe(
        CustomerActor actor,
        Messages.SubscribeDeliveryReq request) {
        customers.subscribe(actor.actorId(), request.deliveryId());
        return new Messages.SubscribeDeliveryRes(request.deliveryId());
    }
}
