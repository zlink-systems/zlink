package systems.zlink.framework.streams;

import systems.zlink.framework.messaging.ZLinkMessage;

import java.util.concurrent.CompletionStage;

public interface ZLinkSessionPacketDispatcher<TSessionContext extends ZLinkSessionContext> {
    CompletionStage<Boolean> tryHandle(
            TSessionContext context, ZLinkSessionDispatchContext dispatch, ZLinkMessage payload);
}
