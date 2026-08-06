package systems.zlink.e2e.spotservice.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class NotifyBoundActorDisconnectedHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext,
        Contracts.NotifyBoundActorDisconnectedReq> {
    @Override
    public Class<Contracts.NotifyBoundActorDisconnectedReq> messageType() {
        return Contracts.NotifyBoundActorDisconnectedReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.NotifyBoundActorDisconnectedReq request) {
        ZLinkSessionActor actor = context.actors().find(request.actorId())
            .orElseThrow(() -> new IllegalStateException(
                "actor is not bound: " + request.actorId()));
        return actor.notifyDisconnected().thenCompose(ignored ->
            context.client().reply(new Contracts.NotifyBoundActorDisconnectedRes(
                request.actorId(), true)).submit());
    }
}
