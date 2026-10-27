package MyGame.match;

import MyGame.*;
import MyGame.match.domain.RoomState;

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
            throw new IllegalArgumentException("Room admission identity mismatch");
        return CompletableFuture.completedFuture(
                state.admit(id)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return actors.sendToActor(actor.context().actorId(), new MatchFound(context.spotId()))
                .submit()
                .thenCompose(ignored -> broadcast(view()));
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private RoomView view() {
        return new RoomView(
                context.spotId(), config.id(), state.view(), state.phase(), state.winner());
    }

    public CompletionStage<Void> sendView(Player actor) {
        return reply(actor, view());
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var rejected = state.move(actor.context().actorId(), request);
        if (rejected != null) return reply(actor, rejected);
        var player =
                state.players().stream()
                        .filter(candidate -> candidate.playerId().equals(actor.context().actorId()))
                        .findFirst()
                        .orElseThrow();
        return reply(actor, new MoveResult(player.position(), request.sequence()))
                .thenCompose(ignored -> broadcast(view()));
    }

    public CompletionStage<Void> shoot(Player actor, ShootRequest request) {
        var decision = state.shoot(actor.context().actorId(), request);
        if (decision.rejected() != null) return reply(actor, decision.rejected());
        return reply(actor, decision.result())
                .thenCompose(ignored -> broadcast(view()))
                .thenCompose(
                        ignored ->
                                state.winner() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : broadcast(new MatchEnded(state.winner())));
    }

    private CompletionStage<Void> broadcast(Object message) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var player : state.players())
            sends =
                    sends.thenCompose(
                            ignored -> actors.sendToActor(player.playerId(), message).submit());
        return sends;
    }

    static CompletionStage<Void> reply(Player actor, Object message) {
        return actor.context().boundSession().send(message).submit();
    }
}
