package MyGame.lobby;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class Party implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final ZLinkSpotManager spots;
    PartyState state = new PartyState();

    public Party(ZLinkSpotContext context, ZLinkActorClient actors, ZLinkSpotManager spots) {
        this.context = context;
        this.actors = actors;
        this.spots = spots;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        var join = payload.decode(PartyJoin.class);
        return CompletableFuture.completedFuture(
                (join.partyId()).equals(context.spotId())
                                && state.join(id, join)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return broadcast();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        state.leave(actor.context().actorId());
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(state.view()).submit();
    }

    public CompletionStage<Void> ready(Player actor) {
        state.markReady(actor.context().actorId());
        return broadcast();
    }

    private CompletionStage<Void> broadcast() {
        var view = state.view();
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.members)
            sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }

    public CompletionStage<Void> enter(Player actor) {
        return enterPlayer(actor.context().actorId());
    }
    public CompletionStage<Void> enterPlayer(String playerId) {
        String reason = state.enter(playerId);
        if (reason != null)
            return actors.sendToActor(playerId, new ActionRejected(reason)).submit();
        if (state.phase == RunPhase.Active) return actors.sendToActor(playerId, state.view()).submit();
        var runId = state.runId;
        String id = Names.dungeonId(runId);
        var members = List.copyOf(state.members);
        var partyId = state.partyId;
        return spots.getOrCreate(id, Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        result -> {
                            if (result.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Dungeon creation rejected");
                            return context.outbound()
                                    .requestToSpot(id, new PrepareDungeon(runId, partyId, members))
                                    .submit(DungeonPrepared.class);
                        })
                .thenCompose(
                        prepared -> {
                            if (!id.equals(prepared.dungeonId()) || state.prepared(prepared.runId()) != null)
                                throw new IllegalStateException(
                                        "Dungeon preparation identity mismatch");
                            CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
                            var travel = new JoinDungeon(id, runId);
                            for (var member : members)
                                if (!state.entered.contains(member)) sent =
                                        sent.thenCompose(
                                                ignored ->
                                                        actors.sendToActor(member, travel)
                                                                .submit());
                            return sent;
                        });
    }
}
