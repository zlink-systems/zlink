package MyGame.match;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Room implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    RoomState state = new RoomState();

    public Room(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        var admission = payload.decode(RoomAdmission.class);
        if (!id.equals(admission.playerId()) || !context.spotId().equals(admission.roomId()))
            throw new IllegalArgumentException("Room identity mismatch");
        return CompletableFuture.completedFuture(
                state.admit(id)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var view = view();
        return actors.sendToActor(actor.context().actorId(), new MatchFound(context.spotId()))
                .submit()
                .thenCompose(ignored -> broadcast(view));
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private RoomView view() {
        return new RoomView(context.spotId(), config.id(), state.view());
    }

    public CompletionStage<Void> sendView(Player actor) {
        return reply(actor, view());
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var reason = state.move(actor.context().actorId(), request);
        return reason == null ? broadcast(view()) : reply(actor, new ActionRejected(reason));
    }

    public CompletionStage<Void> shoot(Player actor, ShootRequest request) {
        var reason = state.shoot(actor.context().actorId(), request);
        if (reason != null) return reply(actor, new ShotRejected(reason));
        var ended = state.winner == null ? null : new MatchEnded(state.winner);
        return broadcast(view())
                .thenCompose(
                        ignored ->
                                ended == null
                                        ? CompletableFuture.completedFuture(null)
                                        : broadcast(ended));
    }

    private CompletionStage<Void> broadcast(Object message) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var player : state.players)
            sends =
                    sends.thenCompose(
                            ignored -> actors.sendToActor(player.playerId, message).submit());
        return sends;
    }

    static CompletionStage<Void> reply(Player actor, Object message) {
        return actor.context().boundSession().send(message).submit();
    }
}
