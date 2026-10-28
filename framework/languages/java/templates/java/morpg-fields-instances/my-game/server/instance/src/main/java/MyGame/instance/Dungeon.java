package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

// Game instances use User Spots so that Player Actors retain their membership.
public final class Dungeon implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    DungeonRun state = new DungeonRun();

    public Dungeon(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public DungeonPrepared prepare(PrepareDungeon request) {
        var rejection = Rules.dungeonId(request.runId()).equals(context.spotId())
                ? state.prepare(request)
                : new ActionRejected(Rejection.RUN_MISMATCH);
        return new DungeonPrepared(context.spotId(), request.runId(), rejection);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(JoinDungeon.class);
        return CompletableFuture.completedFuture(
                context.spotId().equals(join.dungeonId()) && state.canJoin(actorId, join.runId())
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        state.arrived(actor.context().actorId());
        return context.outbound()
                .requestToSpot(
                        state.partyId,
                        new DungeonMemberEntered(state.runId, actor.context().actorId()))
                .submit(PartyCommandResult.class)
                .thenCompose(
                        entered -> {
                            if (entered.rejection() != null)
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(entered.rejection().reason()));
                            if (state.phase != RunPhase.Active)
                                return CompletableFuture.completedFuture(null);
                            return context.outbound()
                                    .requestToSpot(state.partyId, new RunStarted(state.runId))
                                    .submit(PartyCommandResult.class)
                                    .thenCompose(
                                            started -> {
                                                if (started.rejection() != null)
                                                    return CompletableFuture.failedFuture(
                                                            new IllegalStateException(started.rejection().reason()));
                                                return sendToMembers(view());
                                            });
                        });
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private DungeonView view() {
        return new DungeonView(
                context.spotId(), config.id(), state.partyId, state.members,
                state.stage, state.runId, state.phase);
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> advance(Player requester) {
        var rejection = state.advance();
        if (rejection != null)
            return requester.context().boundSession().send(rejection).submit();
        CompletionStage<Void> reported = CompletableFuture.completedFuture(null);
        if (state.phase == RunPhase.Completed)
            reported = context.outbound()
                    .requestToSpot(state.partyId, new RunCompleted(state.runId))
                    .submit(PartyCommandResult.class)
                    .thenCompose(
                            completed -> completed.rejection() == null
                                    ? CompletableFuture.completedFuture(null)
                                    : CompletableFuture.failedFuture(
                                            new IllegalStateException(completed.rejection().reason())));
        var current = view();
        return reported.thenCompose(
                ignored -> requester.context().boundSession().send(current).submit())
                .thenCompose(ignored -> sendToMembersExcept(current, requester.context().actorId()));
    }

    public CompletionStage<Void> enter(Player actor) {
        return show(actor);
    }

    public CompletionStage<Void> rejectTownRequest(Player actor) {
        return actor.context().boundSession()
                .send(new ActionRejected(Rejection.RUN_IN_PROGRESS))
                .submit();
    }

    private CompletionStage<Void> sendToMembers(DungeonView current) {
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.members)
            sent = sent.thenCompose(ignored -> actors.sendToActor(id, current).submit());
        return sent;
    }

    private CompletionStage<Void> sendToMembersExcept(DungeonView current, String excluded) {
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.members)
            if (!id.equals(excluded))
                sent = sent.thenCompose(ignored -> actors.sendToActor(id, current).submit());
        return sent;
    }

    public byte[] capture() {
        try {
            return Json.MAPPER.writeValueAsBytes(state);
        } catch (java.io.IOException error) {
            throw new IllegalStateException("Dungeon serialization failed", error);
        }
    }

    public void restore(byte[] payload) {
        try {
            state = Json.MAPPER.readValue(payload, DungeonRun.class);
        } catch (java.io.IOException error) {
            throw new IllegalStateException("Dungeon relocation state is invalid", error);
        }
    }
}
