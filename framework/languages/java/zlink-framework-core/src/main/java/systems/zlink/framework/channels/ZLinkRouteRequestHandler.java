package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;

public interface ZLinkRouteRequestHandler<TRequest, TReply> {
    CompletionStage<TReply> handle(
        TRequest request,
        ZLinkRouteMessageContext context);
}
