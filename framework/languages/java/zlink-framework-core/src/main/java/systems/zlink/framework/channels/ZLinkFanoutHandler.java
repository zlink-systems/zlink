package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;

public interface ZLinkFanoutHandler<TMessage> {
    CompletionStage<Void> handle(
        TMessage message,
        ZLinkPublishMessageContext context);
}
