package systems.zlink.samples.deliverydispatch.server.customergateway.sessions;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class CustomerSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;

    public CustomerSession(
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
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.allOf(context.actors().bound().stream()
            .map(actor -> actor.notifyDisconnected().toCompletableFuture())
            .toArray(CompletableFuture[]::new));
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled -> {
            if (handled) return CompletableFuture.completedFuture(null);
            ZLinkSessionActor actor = switch (context.actors().bound().size()) {
                case 1 -> context.actors().bound().get(0);
                case 0 -> throw new IllegalStateException(
                    "Client must subscribe before relaying packet '" + dispatch.packetName() + "'");
                default -> throw new IllegalStateException(
                    "Exactly one customer actor must be bound before relaying packet '" + dispatch.packetName() + "'");
            };
            return actor.relay(payload).thenApply(ignored -> null);
        });
    }
}
