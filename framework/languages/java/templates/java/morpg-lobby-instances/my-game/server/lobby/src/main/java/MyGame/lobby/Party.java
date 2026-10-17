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
                (Names.PARTY_PREFIX + join.partyId()).equals(context.spotId())
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
        state.ready.add(actor.context().actorId());
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
        String reason = state.entryRejection(actor.context().actorId());
        if (reason != null)
            return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        String id = Names.DUNGEON_PREFIX + state.partyId;
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
                                    .requestToSpot(id, new PrepareDungeon(partyId, members))
                                    .submit(DungeonPrepared.class);
                        })
                .thenCompose(
                        prepared -> {
                            if (!id.equals(prepared.dungeonId()))
                                throw new IllegalStateException(
                                        "Dungeon preparation identity mismatch");
                            CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
                            var travel = new JoinDungeon(id, partyId);
                            for (var member : members)
                                sent =
                                        sent.thenCompose(
                                                ignored ->
                                                        actors.sendToActor(member, travel)
                                                                .submit());
                            return sent;
                        });
    }
}
