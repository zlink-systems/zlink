package systems.zlink.tutorial.server.sessions;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

// --8<-- [start:session-class]
// One connected game client. Callbacks for the same connection run in order.
// The dispatcher is handed in by the Framework and holds the handlers the
// stream node registered.
public final class GameSession implements ZLinkSession {

    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;

    public GameSession(
        ZLinkSessionContext context,
        ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers) {
        this.context = context;
        this.handlers = handlers;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        System.out.println("client connected: " + context.sessionId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        System.out.println("client disconnected: " + context.sessionId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        System.out.println("stream error on " + context.sessionId() + ": " + error);
        return CompletableFuture.completedFuture(null);
    }

    // Every inbound packet arrives here first. Registering a handler is not
    // enough on its own; this method is what routes the packet to it.
    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled -> {
            if (handled) {
                return CompletableFuture.completedFuture(null);
            }

            // --8<-- [start:session-actor-relay]
            // Anything without a session handler is forwarded to the player bound
            // to this connection, which is why authentication has to come first.
            if (context.actors().bound().size() != 1) {
                throw new IllegalStateException("Authenticate before sending player packets.");
            }

            ZLinkSessionActor player = context.actors().bound().get(0);
            return player.relay(dispatch, payload);
            // --8<-- [end:session-actor-relay]
        });
    }
}
// --8<-- [end:session-class]
