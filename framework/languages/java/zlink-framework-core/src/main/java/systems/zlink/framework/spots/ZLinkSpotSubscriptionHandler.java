package systems.zlink.framework.spots;

import systems.zlink.framework.channels.ZLinkPublishMessageContext;

import java.util.concurrent.CompletionStage;

public interface ZLinkSpotSubscriptionHandler<TSpot, TEvent> {
    CompletionStage<Void> handle(TSpot spot, TEvent message);

    default CompletionStage<Void> handle(
            TSpot spot, TEvent message, ZLinkPublishMessageContext context) {
        return handle(spot, message);
    }
}
