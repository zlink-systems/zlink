package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkRequestHandler<TRequest, TReply> {
    CompletionStage<TReply> handle(
        TRequest request,
        ZLinkMessageContext context);
}
