package systems.zlink.tutorial.server.sessions;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload)
                .thenCompose(
                        handled -> {
                            if (handled) {
                                return CompletableFuture.completedFuture(null);
                            }

                            // --8<-- [start:session-actor-relay]
                            ZLinkSessionActor player = dispatch.actor();
                            if (player == null) {
                                var bound = context.actors().bound();
                                player =
                                        switch (bound.size()) {
                                            case 1 -> bound.getFirst();
                                            case 0 ->
                                                    throw new IllegalStateException(
                                                            "Authenticate an Actor before sending player packets.");
                                            default ->
                                                    throw new IllegalStateException(
                                                            "Select an Actor handle when more than one Actor is bound.");
                                        };
                            }
                            return player.relay(dispatch, payload);
                            // --8<-- [end:session-actor-relay]
                        });
    }
}
// --8<-- [end:session-class]
