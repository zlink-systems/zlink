package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkActor;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkEntrySpotActorSendHandler<
    TEntrySpot extends ZLinkEntrySpot<?>,
    TActor extends ZLinkActor,
    TMessage> {
    CompletionStage<Void> handle(
        TEntrySpot entrySpot,
        TActor actor,
        ZLinkMessageContext context,
        TMessage message);
}
