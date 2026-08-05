package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

public final class ProbeSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;

    public ProbeSession(
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
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.failedFuture(
            new IllegalStateException("stream error: " + error.error()));
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload)
            .thenCompose(handled -> handled
                ? CompletableFuture.completedFuture(null)
                : requireActor(dispatch).relay(dispatch, payload).thenApply(ignored -> null));
    }

    private ZLinkSessionActor requireActor(ZLinkSessionDispatchContext dispatch) {
        String actorId = dispatch.metadata().get("actor-id");
        if (actorId != null && !actorId.isBlank()) {
            return context.actors().find(actorId)
                .orElseThrow(() -> new IllegalStateException("actor is not bound: " + actorId));
        }
        if (context.actors().bound().size() == 1) {
            return context.actors().bound().get(0);
        }
        throw new IllegalStateException("actor-id metadata is required");
    }
}
