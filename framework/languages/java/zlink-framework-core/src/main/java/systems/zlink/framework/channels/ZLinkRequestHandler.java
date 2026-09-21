package systems.zlink.framework.channels;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.concurrent.CompletionStage;

public interface ZLinkRequestHandler<TRequest, TReply> {
    CompletionStage<TReply> handle(TRequest request, ZLinkMessageContext context);
}
