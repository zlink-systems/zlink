package MyGame.match.infrastructure.zlink;

import MyGame.*;
import MyGame.match.domain.RoomRoster;
import MyGame.match.domain.RoomState;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Room implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient clients;
    final RoomRoster roster = new RoomRoster();
    RoomState state = new RoomState();

    public Room(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient clients) {
        this.context = context;
        this.config = config;
        this.clients = clients;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage request) {
        checkJoin(id, request.decode(JoinRoomRequest.class));
        return CompletableFuture.completedFuture(
                roster.canAdmit(id) && state.admit(id)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public RoomPrepared prepare(PrepareRoom request) {
        return roster.prepare(context.spotId(), request);
    }

    void checkJoin(String id, JoinRoomRequest request) {
        if (!id.equals(request.playerId()) || !context.spotId().equals(request.roomId()))
            throw new IllegalArgumentException("Room admission identity mismatch");
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return broadcast(view());
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private RoomView view() {
        return new RoomView(context.spotId(), config.id(), state.view(), state.phase(), state.winner());
    }

    public CompletionStage<Void> current(Player actor) {
        return reply(actor, view());
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var rejected = state.move(actor.context().actorId(), request);
        if (rejected != null) return reply(actor, rejected);
        var player = state.view().stream().filter(p -> p.playerId().equals(actor.context().actorId())).findFirst().orElseThrow();
        return reply(actor, new MoveResult(player.x(), player.y(), request.sequence()))
                .thenCompose(ignored -> broadcast(view()));
    }

    public CompletionStage<Void> shoot(Player actor, ShootRequest request) {
        var rejected = state.shoot(actor.context().actorId(), request);
        if (rejected != null) return reply(actor, rejected);
        return reply(actor, new ShotResult("hit", request.targetId(), request.sequence()))
                .thenCompose(ignored -> broadcast(view()))
                .thenCompose(
                        ignored ->
                                state.winner() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : broadcast(new MatchEnded(state.winner())));
    }

    private CompletionStage<Void> broadcast(Object message) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var player : state.view())
            sends =
                    sends.thenCompose(
                            ignored -> clients.sendToActor(player.playerId(), message).submit());
        return sends;
    }

    static CompletionStage<Void> reply(Player actor, Object message) {
        return actor.context().boundSession().send(message).submit();
    }
}
