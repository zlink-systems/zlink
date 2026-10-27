package MyGame.session;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.*;

import java.util.Set;
import java.util.concurrent.*;

public final class GameSession implements ZLinkSession {
    private static final Set<String> CLIENT_PACKETS =
            Set.of(
                    JoinTableRequest.class.getSimpleName(),
                    TableViewRequest.class.getSimpleName(),
                    StartHandRequest.class.getSimpleName(),
                    BetRequest.class.getSimpleName(),
                    SessionInfoRequest.class.getSimpleName());

    private final ZLinkSessionContext context;
    private final ZLinkActorManager actors;

    public GameSession(ZLinkSessionContext context, ZLinkActorManager actors) {
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
        if (!CLIENT_PACKETS.contains(dispatch.packetName()))
            return context.client()
                    .send(new ActionRejected(Rules.PACKET_NOT_ALLOWED, 0))
                    .submit();

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
        if (dispatch.packetName().equals(JoinTableRequest.class.getSimpleName())) {
            var request = payload.decode(JoinTableRequest.class);
            if (request.playerId() == null
                    || request.playerId().isBlank()
                    || request.tableId() == null
                    || request.tableId().isBlank())
                throw new IllegalArgumentException("Player and table IDs are required");
            return actors.getOrCreate(request.playerId(), Names.PLAYER)
                    .inMesh(Names.MESH)
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
            throw new IllegalStateException("JoinTableRequest must be the first packet");
        return bound.getFirst();
    }
}
