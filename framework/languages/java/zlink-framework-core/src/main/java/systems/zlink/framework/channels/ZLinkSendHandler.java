package systems.zlink.framework.channels;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.concurrent.CompletionStage;

public interface ZLinkSendHandler<TMessage> {
    CompletionStage<Void> handle(TMessage message, ZLinkMessageContext context);
}
