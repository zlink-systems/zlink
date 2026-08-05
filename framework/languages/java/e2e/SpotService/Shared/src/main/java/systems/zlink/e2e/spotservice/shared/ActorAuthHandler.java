package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ActorAuthHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ActorAuthReq> {
    private final ZLinkActorManager actors;
    private final ScenarioState evidence;

    public ActorAuthHandler(
        ZLinkActorManager actors,
        ScenarioState evidence) {
        this.actors = actors;
        this.evidence = evidence;
    }

    @Override
    public Class<Contracts.ActorAuthReq> messageType() {
        return Contracts.ActorAuthReq.class;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.ActorAuthReq request) {
        return actors.find(request.actorId())
            .thenCompose(existing -> existing.<java.util.concurrent.CompletionStage<ActorRef>>map(
                java.util.concurrent.CompletableFuture::completedFuture)
                .orElseGet(() -> actors.create(request.actorId(), "scenario")
                    .request(request)
                    .submit()
                    .thenApply(ActorAuthHandler::createdActor)))
            .thenCompose(actor -> context.actors().bind(actor))
            .thenAccept(bound -> {
                evidence.record("ActorSessionBound", "session", request.actorId());
                context.client().reply(new Contracts.ActorAuthRes(
                    bound.actorId(), bound.ref().nodeRid().toString(), bound.ref().objectGeneration(),
                    context.actors().bound().size(),
                    request.profile().displayName(), request.profile().level(), request.profile().tags())).submit();
            });
    }

    private static ActorRef createdActor(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        throw new IllegalStateException("actor creation was rejected");
    }
}
