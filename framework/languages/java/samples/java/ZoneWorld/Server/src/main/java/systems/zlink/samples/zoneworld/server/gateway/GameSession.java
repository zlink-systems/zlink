package systems.zlink.samples.zoneworld.server.gateway;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class GameSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkActorManager actors;

    public GameSession(
        ZLinkSessionContext context,
        ZLinkActorManager actors) {
        this.context = context;
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
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        if ("JoinWorldReq".equals(dispatch.packetName())) {
            return join(dispatch, payload);
        }
        if (context.actors().bound().size() != 1) {
            throw new IllegalStateException("JoinWorldReq must bind an actor before game traffic");
        }
        return context.actors().bound().get(0).relay(dispatch, payload);
    }

    private CompletionStage<Void> join(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        Messages.JoinWorldReq request = payload.decode(Messages.JoinWorldReq.class);
        return actors.getOrCreate(request.playerId(), ZoneWorldNames.PLAYER_ACTOR_TYPE)
            .inMesh(ZoneWorldNames.MESH)
            .request(new Messages.EnterWorldReq(
                ZoneWorldSpec.SPAWN_X,
                ZoneWorldSpec.SPAWN_Y,
                false,
                0,
                0))
            .submit()
            .thenCompose(result -> {
                if (result instanceof ZLinkActorCreateResult.Rejected rejected) {
                    return CompletableFuture.failedFuture(new IllegalStateException(
                        rejected.reply().decode(Messages.EnterWorldRes.class).error()));
                }
                ActorRef actorRef = result instanceof ZLinkActorCreateResult.Created created
                    ? created.actor()
                    : ((ZLinkActorCreateResult.Existing) result).actor();
                return context.actors().bindOrGet(actorRef)
                    .thenCompose(actor -> actor.relay(dispatch, payload));
            });
    }
}
