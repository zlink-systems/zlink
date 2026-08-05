package systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.handlers;

import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.CourierActor;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class BindCourierSessionActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        CourierEntrySpot,
        CourierActor,
        Messages.BindCourierSessionReq,
        Messages.BindCourierSessionRes> {
    @Override
    public CompletionStage<Messages.BindCourierSessionRes> handle(
        CourierEntrySpot entrySpot,
        CourierActor actor,
        ZLinkMessageContext context,
        Messages.BindCourierSessionReq request) {
        System.out.println("courier-bind-relayed=" + request.courierId());
        return CompletableFuture.completedFuture(new Messages.BindCourierSessionRes(
            request.courierId(),
            request.actor(),
            request.sessionRoute()));
    }
}
