package systems.zlink.e2e.observabilityops.session;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ForceReconnectSessionHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, ForceReconnectSessionHandler.ForceReconnectReq> {
    @Override
    public Class<ForceReconnectReq> messageType() {
        return ForceReconnectReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ForceReconnectReq request) {
        return context.close();
    }

    public record ForceReconnectReq(int cycle) { }
}
