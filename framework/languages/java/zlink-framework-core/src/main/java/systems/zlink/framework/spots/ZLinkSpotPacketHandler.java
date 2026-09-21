package systems.zlink.framework.spots;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.concurrent.CompletionStage;

public interface ZLinkSpotPacketHandler<TSpot, TMessage> {
    CompletionStage<Void> handle(TSpot spot, TMessage message);

    default CompletionStage<Void> handle(
            TSpot spot, TMessage message, ZLinkMessageContext context) {
        return handle(spot, message);
    }
}
