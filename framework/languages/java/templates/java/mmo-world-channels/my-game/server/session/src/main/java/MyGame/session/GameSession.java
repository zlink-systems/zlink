package MyGame.session;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.*;

import java.util.concurrent.*;

public final class GameSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkActorManager actors;
    private final NodeConfig config;

    public GameSession(ZLinkSessionContext context, ZLinkActorManager actors, NodeConfig config) {
        this.config = config;
        this.context = context;
        this.actors = actors;
    }

    public ZLinkSessionContext context() {
        return context;
    }

    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.failedFuture(new IllegalStateException(error.toString()));
    }

    public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        if (dispatch.packetName().equals(SessionInfoRequest.class.getSimpleName())) {
            var actor = bound();
            return context.client()
                    .reply(
                            new SessionInfo(
                                    context.sessionId(),
                                    actor.actorId(),
                                    actor.ref().objectGeneration()))
                    .submit();
        }
        if (dispatch.packetName().equals(EnterWorldRequest.class.getSimpleName())) {
            var request = payload.decode(EnterWorldRequest.class);
            if (request.playerId() == null || request.playerId().isBlank())
                throw new IllegalArgumentException("Player ID is required");
            return actors.getOrCreate(request.playerId(), Names.PLAYER)
                    .inMesh(Names.mesh(config.worldId()))
                    .request(ZLinkMessage.empty())
                    .submit()
                    .thenCompose(
                            result -> {
                                ActorRef actor =
                                        switch (result) {
                                            case ZLinkActorCreateResult.Created created ->
                                                    created.actor();
                                            case ZLinkActorCreateResult.Existing existing ->
                                                    existing.actor();
                                            case ZLinkActorCreateResult.Rejected rejected ->
                                                    throw new IllegalStateException(
                                                            "Actor creation rejected: " + rejected);
                                        };
                                return context.actors()
                                        .bindOrGet(actor)
                                        .thenCompose(bound -> bound.relay(dispatch, payload));
                            });
        }
        return bound().relay(dispatch, payload);
    }

    private ZLinkSessionActor bound() {
        var bound = context.actors().bound();
        if (bound.size() != 1)
            throw new IllegalStateException("EnterWorldRequest must be the first packet");
        return bound.getFirst();
    }
}
