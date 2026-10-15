package MyGame.match.infrastructure.zlink;

import MyGame.*;
import MyGame.match.domain.RoomState;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Room implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient clients;
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
                state.admit(id)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
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
        return new RoomView(context.spotId(), config.id(), state.view());
    }

    public CompletionStage<Void> current(Player actor) {
        return reply(actor, view());
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        String reason = state.move(actor.context().actorId(), request);
        return reason == null ? broadcast(view()) : reply(actor, new ActionRejected(reason));
    }

    public CompletionStage<Void> shoot(Player actor, ShootRequest request) {
        String reason = state.shoot(actor.context().actorId(), request);
        if (reason != null) return reply(actor, new ShotRejected(reason));
        return broadcast(view())
                .thenCompose(
                        ignored ->
                                state.winner == null
                                        ? CompletableFuture.completedFuture(null)
                                        : broadcast(new MatchEnded(state.winner)));
    }

    private CompletionStage<Void> broadcast(Object message) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var player : state.players)
            sends =
                    sends.thenCompose(
                            ignored -> clients.sendToActor(player.playerId, message).submit());
        return sends;
    }

    static CompletionStage<Void> reply(Player actor, Object message) {
        return actor.context().boundSession().send(message).submit();
    }
}
