package systems.zlink.samples.deliverydispatch.server.couriersession.sessions;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class CourierSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
    private final ZLinkActorManager actors;

    public CourierSession(
        ZLinkSessionContext context,
        ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers,
        ZLinkActorManager actors) {
        this.context = context;
        this.handlers = handlers;
        this.actors = actors;
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
        CompletableFuture<?>[] notifications = context.actors().bound().stream()
            .map(actor -> actor.notifyDisconnected().toCompletableFuture())
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(notifications);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        if ("BindCourierSessionReq".equals(dispatch.packetName())) {
            return handleBindCourierSessionReq(dispatch, payload);
        }
        return handlers.tryHandle(context, dispatch, payload).thenCompose(handled -> {
            if (handled) {
                return CompletableFuture.completedFuture(null);
            }
            Messages.CourierDecision decision = payload.decode(Messages.CourierDecision.class);
            ZLinkSessionActor actor = context.actors().find(decision.courierId())
                .orElseThrow(() -> new IllegalStateException(
                    "Courier actor is not bound: " + decision.courierId()));
            return actor.relay(payload).thenApply(ignored -> null);
        });
    }

    private CompletionStage<Void> handleBindCourierSessionReq(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        Messages.BindCourierSessionReq request = payload.decode(Messages.BindCourierSessionReq.class);
        return findOrEnsureActor(request.courierId()).thenCompose(actorRef -> {
            ZLinkSessionActor bound = context.actors().find(actorRef.actorId()).orElse(null);
            CompletionStage<ZLinkSessionActor> actorStage = bound == null
                ? context.actors().bind(actorRef)
                : CompletableFuture.completedFuture(bound);
            ActorRefSnapshot snapshot = ActorRefSnapshot.from(actorRef);
            Messages.BindCourierSessionReq relayed = new Messages.BindCourierSessionReq(
                request.courierId(), snapshot, context.sessionId());
            return actorStage.thenCompose(actor ->
                actor.relay(dispatch, ZLinkMessage.of(relayed)).thenApply(ignored -> null));
        });
    }

    private CompletionStage<ActorRef> findOrEnsureActor(
        String courierId) {
        return actors.getOrCreate(courierId, SampleNames.CourierActorType)
            .request(new Messages.EnsureCourierActorReq(courierId))
            .submit()
            .thenApply(result -> {
                if (result instanceof ZLinkActorCreateResult.Existing existing) {
                    return existing.actor();
                }
                if (result instanceof ZLinkActorCreateResult.Created created) {
                    return created.actor();
                }
                throw new IllegalStateException("Courier Actor creation was rejected.");
            });
    }
}
