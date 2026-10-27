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
        if (!Names.dungeonId(request.runId()).equals(context.spotId()) || state.prepare(request) != null) throw new IllegalArgumentException(Rules.RUN_MISMATCH);
        return new DungeonPrepared(context.spotId(), request.runId());
    }
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        return CompletableFuture.completedFuture(state.admit(id, payload.decode(JoinDungeon.class), context.spotId()) ? ZLinkSpotActorJoinResult.accept() : ZLinkSpotActorJoinResult.reject());
    }
    public CompletionStage<Void> onJoinedActor(Player actor) {
        boolean started = state.join(actor.context().actorId());
        return context.outbound().sendToSpot(state.partyId, new DungeonMemberEntered(state.runId, actor.context().actorId())).submit()
            .thenCompose(ignored -> started ? context.outbound().sendToSpot(state.partyId, new RunStarted(state.runId)).submit() : CompletableFuture.<Void>completedFuture(null))
            .thenCompose(ignored -> broadcast());
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private DungeonView view() {
        return new DungeonView(
                context.spotId(), config.id(), state.partyId, state.members, state.stage, state.runId, state.phase);
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> enter(Player actor) { return context.outbound().sendToSpot(state.partyId, new EnterPartyRun(actor.context().actorId())).submit(); }
    public CompletionStage<Void> advance(Player actor) {
        String reason = state.advance();
        if (reason != null) return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        CompletionStage<Void> completed = state.phase == RunPhase.Completed
            ? context.outbound().sendToSpot(state.partyId, new RunCompleted(state.runId)).submit()
            : CompletableFuture.completedFuture(null);
        return completed.thenCompose(ignored -> broadcast());
    }
    private CompletionStage<Void> broadcast() {
        var view = view(); CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.joined) sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }
}
