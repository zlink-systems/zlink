package systems.zlink.samples.bingo.server.session.sessions;


import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;

public final class BingoSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;

    public BingoSession(
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
    public java.util.concurrent.CompletionStage<Void> onConnected() {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnected() {
        return java.util.concurrent.CompletableFuture.allOf(context.actors().bound().stream()
            .map(actor -> actor.notifyDisconnected().toCompletableFuture())
            .toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onError(ZLinkStreamError error) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled ->
            handled
                ? java.util.concurrent.CompletableFuture.completedFuture(null)
                : requireSingleBoundActor(dispatch.packetName()).relay(payload).thenApply(ignored -> null));
    }

    private ZLinkSessionActor requireSingleBoundActor(String packetName) {
        return switch (context.actors().bound().size()) {
            case 1 -> context.actors().bound().get(0);
            case 0 -> throw new IllegalStateException(
                "Client must authenticate before relaying packet '" + packetName + "'");
            default -> throw new IllegalStateException(
                "Exactly one actor must be bound before relaying packet '" + packetName + "'");
        };
    }
}
