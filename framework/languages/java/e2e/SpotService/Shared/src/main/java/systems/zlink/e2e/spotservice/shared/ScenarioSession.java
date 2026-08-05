package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

public final class ScenarioSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
    private final ScenarioState evidence;

    public ScenarioSession(
        ZLinkSessionContext context,
        ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers,
        ScenarioState evidence) {
        this.context = context;
        this.handlers = handlers;
        this.evidence = evidence;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onConnected() {
        evidence.record("StreamConnected", "session", context.sessionId());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnected() {
        evidence.record("StreamDisconnected", "session", context.sessionId());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onError(ZLinkStreamError error) {
        evidence.record("StreamError", "session", error.error().name());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        evidence.record("StreamInbound", "session", dispatch.packetName());
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled ->
            handled ? java.util.concurrent.CompletableFuture.completedFuture(null)
                : requireActor(dispatch).relay(dispatch, payload));
    }

    private ZLinkSessionActor requireActor(ZLinkSessionDispatchContext dispatch) {
        String actorId = dispatch.metadata().get("actor-id");
        if (actorId != null && !actorId.isBlank()) {
            return context.actors().find(actorId)
                .orElseThrow(() -> new IllegalStateException("actor is not bound: " + actorId));
        }
        return switch (context.actors().bound().size()) {
            case 1 -> context.actors().bound().get(0);
            case 0 -> throw new IllegalStateException("ActorAuthReq is required before actor packet");
            default -> throw new IllegalStateException("actor-id metadata is required for multiple bound actors");
        };
    }
}
