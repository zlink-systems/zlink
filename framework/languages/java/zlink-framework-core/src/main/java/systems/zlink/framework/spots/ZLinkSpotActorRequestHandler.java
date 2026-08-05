package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkActor;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkSpotActorRequestHandler<
    TSpot extends ZLinkSpot<?>,
    TActor extends ZLinkActor,
    TRequest,
    TReply> {
    CompletionStage<TReply> handle(
        TSpot spot,
        TActor actor,
        ZLinkMessageContext context,
        TRequest request);
}
