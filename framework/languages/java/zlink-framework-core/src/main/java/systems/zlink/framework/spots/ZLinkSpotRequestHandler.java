package systems.zlink.framework.spots;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.concurrent.CompletionStage;

public interface ZLinkSpotRequestHandler<TSpot, TRequest, TReply> {
    CompletionStage<TReply> handle(TSpot spot, TRequest request);

    default CompletionStage<TReply> handle(
            TSpot spot, TRequest request, ZLinkMessageContext context) {
        return handle(spot, request);
    }
}
