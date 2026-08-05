package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.sessions;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;

// --8<-- [start:doc-session]
public final class PlaySession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;

    public PlaySession(
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
        ZLinkSessionDispatchContext header,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, header, payload).thenCompose(handled ->
            handled
                ? java.util.concurrent.CompletableFuture.completedFuture(null)
                : requireActor(header.packetName()).relay(header, payload).thenApply(ignored -> null));
    }

    private ZLinkSessionActor requireActor(String packetName) {
        return switch (context.actors().bound().size()) {
            case 1 -> context.actors().bound().get(0);
            case 0 -> throw new IllegalStateException(
                "AuthenticateReq is required before play packet '" + packetName + "'");
            default -> throw new IllegalStateException(
                "Exactly one actor must be bound before play packet '" + packetName + "'");
        };
    }
}
// --8<-- [end:doc-session]
