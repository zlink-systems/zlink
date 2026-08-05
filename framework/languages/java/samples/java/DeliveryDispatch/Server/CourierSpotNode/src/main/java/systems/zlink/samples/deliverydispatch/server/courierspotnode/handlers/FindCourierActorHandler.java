package systems.zlink.samples.deliverydispatch.server.courierspotnode.handlers;

import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletionStage;

public final class FindCourierActorHandler
    implements ZLinkSpotRequestHandler<CourierEntrySpot, Messages.FindCourierActorReq, Messages.FindCourierActorRes> {
    private final ZLinkActorManager actors;

    public FindCourierActorHandler(ZLinkActorManager actors) {
        this.actors = actors;
    }

    @Override
    public CompletionStage<Messages.FindCourierActorRes> handle(
        CourierEntrySpot spot,
        Messages.FindCourierActorReq request) {
        return actors.find(request.courierId()).thenApply(found -> found
            .map(actor -> new Messages.FindCourierActorRes(
                request.courierId(), systems.zlink.framework.actors.ActorRefSnapshot.from(actor)))
            .orElseGet(() -> new Messages.FindCourierActorRes(request.courierId(), null)));
    }
}
