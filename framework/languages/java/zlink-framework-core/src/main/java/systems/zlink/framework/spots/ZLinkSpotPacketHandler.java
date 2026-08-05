package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkSpotPacketHandler<TSpot, TMessage> {
    CompletionStage<Void> handle(TSpot spot, TMessage message);

    default CompletionStage<Void> handle(
        TSpot spot,
        TMessage message,
        ZLinkMessageContext context) {
        return handle(spot, message);
    }
}
