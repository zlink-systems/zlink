package systems.zlink.framework.streams;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkSessionPacketDispatcher<TSessionContext extends ZLinkSessionContext> {
    CompletionStage<Boolean> tryHandle(
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload);
}
