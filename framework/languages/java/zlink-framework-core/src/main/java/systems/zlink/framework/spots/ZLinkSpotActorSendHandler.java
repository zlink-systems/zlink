package systems.zlink.framework.spots;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;

import java.util.concurrent.CompletionStage;

public interface ZLinkSpotActorSendHandler<
        TSpot extends ZLinkSpot<?>, TActor extends ZLinkActor, TMessage> {
    CompletionStage<Void> handle(
            TSpot spot, TActor actor, ZLinkMessageContext context, TMessage message);
}
