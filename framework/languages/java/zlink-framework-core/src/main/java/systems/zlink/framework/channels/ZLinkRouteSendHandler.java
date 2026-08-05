package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;

public interface ZLinkRouteSendHandler<TMessage> {
    CompletionStage<Void> handle(
        TMessage message,
        ZLinkRouteMessageContext context);
}
