package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class Dungeon implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    DungeonState state = new DungeonState();

    public Dungeon(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public DungeonPrepared prepare(PrepareDungeon request) {
        String reason = state.prepare(context.spotId(), request);
        return new DungeonPrepared(context.spotId(), request.runId(), reason);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        var join = payload.decode(JoinDungeon.class);
        return CompletableFuture.completedFuture(
                state.accepts(id, context.spotId(), join)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        CompletionStage<Void> started = state.join(actor.context().actorId())
                ? context.outbound().sendToSpot(state.ownerSpotId, new RunStarted(state.runId)).submit()
                : CompletableFuture.completedFuture(null);
        return started.thenCompose(ignored -> broadcast());
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private DungeonView view() {
        return new DungeonView(
                context.spotId(),
                config.id(),
                state.partyId,
                state.members,
                state.stage,
                state.runId,
                state.phase);
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> showWithParty(Player actor) {
        return context.outbound()
                .sendToSpot(state.ownerSpotId, new PartyViewRequest(actor.context().actorId()))
                .submit()
                .thenCompose(ignored -> show(actor));
    }

    private CompletionStage<Void> broadcast() {
        var current = view();
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.joined)
            sent = sent.thenCompose(ignored -> actors.sendToActor(id, current).submit());
        return sent;
    }

    public CompletionStage<Void> advance(Player actor) {
        String reason = state.advance();
        if (reason != null)
            return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        return broadcast().thenCompose(
                ignored -> state.phase == RunPhase.Completed
                        ? context.outbound()
                                .sendToSpot(state.ownerSpotId, new RunCompleted(state.runId))
                                .submit()
                        : CompletableFuture.completedFuture(null));
    }
}
