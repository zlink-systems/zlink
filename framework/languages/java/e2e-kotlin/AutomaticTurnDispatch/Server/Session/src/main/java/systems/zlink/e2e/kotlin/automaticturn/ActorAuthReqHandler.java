package systems.zlink.e2e.kotlin.automaticturn;

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
        return actors.getOrCreate(request.actorId(), "probe", request)
            .thenCompose(context.actors()::bind)
            .thenRun(() -> context.client()
                .reply(new Contracts.ActorAuthRes(request.actorId()))
                .submit());
    }
}
