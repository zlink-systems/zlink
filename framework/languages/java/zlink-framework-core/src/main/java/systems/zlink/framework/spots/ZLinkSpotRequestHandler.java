package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkSpotRequestHandler<TSpot, TRequest, TReply> {
    CompletionStage<TReply> handle(TSpot spot, TRequest request);

    default CompletionStage<TReply> handle(
        TSpot spot,
        TRequest request,
        ZLinkMessageContext context) {
        return handle(spot, request);
    }
}
