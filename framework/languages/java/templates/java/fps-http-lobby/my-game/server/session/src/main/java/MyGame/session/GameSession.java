package MyGame.session;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.*;

import java.util.Set;
import java.util.concurrent.*;

public final class GameSession implements ZLinkSession {
    private static final Set<String> ALLOWED =
            Set.of(
                    JoinRoomRequest.class.getSimpleName(),
                    RoomViewRequest.class.getSimpleName(),
                    MoveRequest.class.getSimpleName(),
                    ShootRequest.class.getSimpleName(),
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
        String packetName = dispatch.packetName();
        if (!ALLOWED.contains(packetName))
            return context.client().send(new ActionRejected(Rejection.PACKET_NOT_ALLOWED)).submit();

        if (context.actors().bound().isEmpty()) {
            if (!packetName.equals(JoinRoomRequest.class.getSimpleName()))
                return CompletableFuture.failedFuture(
                        new IllegalStateException("JoinRoomRequest must be the first player packet"));
            var request = payload.decode(JoinRoomRequest.class);
            if (request.playerId() == null || request.playerId().isBlank()
                    || request.roomId() == null || request.roomId().isBlank())
                return CompletableFuture.failedFuture(
                        new IllegalArgumentException("Player and room IDs are required"));
            return actors.getOrCreate(request.playerId(), Names.PLAYER)
                    .inMesh(Names.MESH)
                    .request(ZLinkMessage.empty())
                    .submit()
                    .thenCompose(
                            result -> {
                                ActorRef actor =
                                        switch (result) {
                                            case ZLinkActorCreateResult.Created created -> created.actor();
                                            case ZLinkActorCreateResult.Existing existing -> existing.actor();
                                            case ZLinkActorCreateResult.Rejected rejected ->
                                                    throw new IllegalStateException(
                                                            "Actor creation rejected: " + rejected);
                                        };
                                return context.actors()
                                        .bindOrGet(actor)
                                        .thenCompose(bound -> bound.relay(dispatch, payload));
                            });
        }
        if (packetName.equals(SessionInfoRequest.class.getSimpleName())) {
            var actor = bound();
            return context.client()
                    .reply(
                            new SessionInfo(
                                    context.sessionId(),
                                    actor.actorId(),
                                    actor.ref().objectGeneration()))
                    .submit();
        }
        return bound().relay(dispatch, payload);
    }

    private ZLinkSessionActor bound() {
        var bound = context.actors().bound();
        if (bound.size() != 1)
            throw new IllegalStateException("Exactly one player Actor must be bound");
        return bound.getFirst();
    }
}
