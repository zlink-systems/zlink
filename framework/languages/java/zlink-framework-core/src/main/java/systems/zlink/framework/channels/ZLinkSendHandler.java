package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkSendHandler<TMessage> {
    CompletionStage<Void> handle(
        TMessage message,
        ZLinkMessageContext context);
}
