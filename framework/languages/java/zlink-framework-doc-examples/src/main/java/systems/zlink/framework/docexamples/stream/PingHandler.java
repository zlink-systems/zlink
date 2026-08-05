package systems.zlink.framework.docexamples.stream;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

/** 가이드 9장 §3 — typed packet handler. */
// --8<-- [start:typed-packet-handler]
public final class PingHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, StreamMessages.Ping> {

    @Override
    public Class<StreamMessages.Ping> messageType() {
        return StreamMessages.Ping.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        StreamMessages.Ping message) {
        if (!dispatch.canReply()) {
            throw new IllegalStateException("Ping must be a request.");
        }

        // 같은 request correlation으로 한 번만 reply한다.
        return context.client().reply(new StreamMessages.Pong(message.sequence())).submit();
    }
}
// --8<-- [end:typed-packet-handler]
