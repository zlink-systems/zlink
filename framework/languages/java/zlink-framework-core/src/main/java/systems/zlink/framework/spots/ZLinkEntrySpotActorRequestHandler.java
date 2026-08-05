package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkActor;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkEntrySpotActorRequestHandler<
    TEntrySpot extends ZLinkEntrySpot<?>,
    TActor extends ZLinkActor,
    TRequest,
    TReply> {
    CompletionStage<TReply> handle(
        TEntrySpot entrySpot,
        TActor actor,
        ZLinkMessageContext context,
        TRequest request);
}
