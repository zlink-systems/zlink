package MyGame.session;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.streams.*;

import java.util.concurrent.*;

public final class GameSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkActorManager actors;

    private final ZLinkActorClient actorClient;
    private final ZLinkSpotManager spots;

    public GameSession(
            ZLinkSessionContext context,
            ZLinkActorManager actors,
            ZLinkActorClient actorClient,
            ZLinkSpotManager spots) {
        this.context = context;
        this.actors = actors;
        this.actorClient = actorClient;
        this.spots = spots;
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
        if (!java.util.Set.of(
                        "JoinWorldRequest",
                        "MoveRequest",
                        "CreatePartyRequest",
                        "JoinPartyRequest",
                        "EnterDungeonRequest",
                        "AdvanceRequest",
                        "ViewRequest",
                        "LeaveDungeonRequest",
                        "SessionInfoRequest")
                .contains(dispatch.packetName())) {
            return context.client().send(new ActionRejected(Rejection.PACKET_NOT_ALLOWED)).submit();
        }
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
        if (dispatch.packetName().equals(JoinWorldRequest.class.getSimpleName())) {
            var request = payload.decode(JoinWorldRequest.class);
            if (request.playerId() == null || request.playerId().isBlank())
                throw new IllegalArgumentException("Player ID is required");
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
        if (dispatch.packetName().equals(LeaveDungeonRequest.class.getSimpleName())) {
            return leaveDungeon();
        }
        return bound().relay(dispatch, payload);
    }

    private CompletionStage<Void> leaveDungeon() {
        return actorClient
                .requestToActor(bound().actorId(), new LeaveDungeonRequest())
                .submit(DungeonLeft.class)
                .thenCompose(
                        left ->
                                // Close outside the Dungeon turn, after its membership change
                                // completes.
                                spots.find(left.dungeonId())
                                        .thenCompose(
                                                found -> {
                                                    if (found.isPresent()
                                                            && found.get().objectGeneration()
                                                                    == left.objectGeneration()) {
                                                        return spots.close(found.get());
                                                    }
                                                    return CompletableFuture.completedFuture(false);
                                                })
                                        .thenCompose(
                                                closed -> context.client().reply(left).submit()));
    }

    private ZLinkSessionActor bound() {
        var bound = context.actors().bound();
        if (bound.size() != 1)
            throw new IllegalStateException("JoinWorldRequest must be the first packet");
        return bound.getFirst();
    }
}
