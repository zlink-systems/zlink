package systems.zlink.samples.bingo.server.session.sessions;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;


import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;

public final class BingoSession implements ZLinkSession {
    private static final Logger logger = LoggerFactory.getLogger(BingoSession.class);

    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
    private String boundActorId;

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
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.allOf(context.actors().bound().stream()
            .map(actor -> actor.notifyDisconnected().toCompletableFuture())
            .toArray(CompletableFuture[]::new))
            .thenRun(() -> {
                if (boundActorId != null) {
                    logger.info(
                        "bingo-lifecycle session-disconnect actor={} destroy=false",
                        boundActorId);
                }
            });
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled ->
            handled ? rememberBoundActor() : requireSingleBoundActor(dispatch.packetName())
                .relay(payload)
                .thenApply(ignored -> null));
    }

    private CompletionStage<Void> rememberBoundActor() {
        if (context.actors().bound().size() == 1) {
            boundActorId = context.actors().bound().getFirst().actorId();
        }
        return CompletableFuture.completedFuture(null);
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
