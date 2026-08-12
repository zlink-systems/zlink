package systems.zlink.e2e.kotlin.automaticturn;
import systems.zlink.framework.actors.ZLinkActorCreateResult;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ActorAuthReqHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ActorAuthReq> {
    private final ZLinkActorManager actors;

    public ActorAuthReqHandler(ZLinkActorManager actors) {
        this.actors = actors;
    }

    @Override
    public Class<Contracts.ActorAuthReq> messageType() {
        return Contracts.ActorAuthReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.ActorAuthReq request) {
        return actors.getOrCreate(request.actorId(), "probe")
            .request(request)
            .submit()
            .thenCompose(result -> context.actors().bind(switch (result) {
                case ZLinkActorCreateResult.Existing existing -> existing.actor();
                case ZLinkActorCreateResult.Created created -> created.actor();
                case ZLinkActorCreateResult.Rejected rejected ->
                    throw new IllegalStateException("actor creation rejected");
            }))
            .thenRun(() -> context.client()
                .reply(new Contracts.ActorAuthRes(request.actorId()))
                .submit());
    }
}
