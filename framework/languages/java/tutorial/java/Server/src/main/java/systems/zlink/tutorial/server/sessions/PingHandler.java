package systems.zlink.tutorial.server.sessions;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:session-handler]
// The first type argument is the session context, not the session class.
// messageType() is what pairs the handler with the packet the client sends.
public final class PingHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.Ping> {

    @Override
    public Class<Contracts.Ping> messageType() {
        return Contracts.Ping.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.Ping message) {
        // reply answers a request. To push to a client that is not waiting for
        // one, use client().send instead.
        return context.client()
            .reply(new Contracts.Pong(message.sentAtUnixMs()))
            .submit();
    }
}
// --8<-- [end:session-handler]
