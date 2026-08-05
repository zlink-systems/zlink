package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class MultiBindHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.MultiBindReq> {
    private final ZLinkActorManager actors;
    private final ScenarioState evidence;

    public MultiBindHandler(
        ZLinkActorManager actors,
        ScenarioState evidence) {
        this.actors = actors;
        this.evidence = evidence;
    }

    @Override
    public Class<Contracts.MultiBindReq> messageType() {
        return Contracts.MultiBindReq.class;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.MultiBindReq request) {
        return bind(context, request.firstActorId(), request)
            .thenCompose(ignored -> bind(context, request.secondActorId(), request))
            .thenRun(() -> context.client()
                .reply(new Contracts.MultiBindRes(context.actors().bound().size())).submit());
    }

    private java.util.concurrent.CompletionStage<Void> bind(
        ZLinkSessionContext context, String actorId, Contracts.MultiBindReq request) {
        return actors.getOrCreate(actorId, "scenario")
            .request(new Contracts.ActorAuthReq(actorId, request.profile()))
            .submit()
            .thenApply(MultiBindHandler::createdActor)
            .thenCompose(context.actors()::bind)
            .thenAccept(ignored -> evidence.record("ActorSessionBound", "session", actorId));
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
