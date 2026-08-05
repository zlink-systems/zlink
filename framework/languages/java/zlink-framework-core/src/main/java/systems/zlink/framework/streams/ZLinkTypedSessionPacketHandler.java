package systems.zlink.framework.streams;

import java.util.concurrent.CompletionStage;

public interface ZLinkTypedSessionPacketHandler<TSessionContext extends ZLinkSessionContext, TMessage> {
    Class<TMessage> messageType();

    CompletionStage<Void> handle(
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        TMessage message);
}
