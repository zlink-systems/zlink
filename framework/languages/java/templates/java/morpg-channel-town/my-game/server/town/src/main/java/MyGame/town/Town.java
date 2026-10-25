package MyGame.town;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class Town implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    private final ZLinkSpotManager spots;
    TownState state = new TownState();

    public Town(
            ZLinkSpotContext context,
            NodeConfig config,
            ZLinkActorClient actors,
            ZLinkSpotManager spots) {
        this.context = context;
        this.config = config;
        this.actors = actors;
        this.spots = spots;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        var join = payload.decode(JoinTownRequest.class);
        if (!id.equals(join.playerId())
                || ChannelRules.validate(join.channel()) != null
                || !Names.townId(join.channel()).equals(context.spotId()))
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        state.players.add(id);
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return broadcast();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        state.leave(actor.context().actorId());
        return broadcast();
    }

    // Disconnect preserves town and party membership until an explicit move.
    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private TownView view() {
        return state.view(context.spotId(), config.id());
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> showParty(String playerId) {
        return actors.sendToActor(playerId, view()).submit();
    }

    private CompletionStage<Void> broadcast() {
        var view = view();
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.players)
            sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }

    public CompletionStage<Void> party(Player actor, String partyId, boolean create) {
        if (!state.party(actor.context().actorId(), partyId, create))
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rules.INVALID_PARTY))
                    .submit();
        return broadcast();
    }

    public CompletionStage<Void> changeChannel(Player actor, long channel) {
        var party = state.partyFor(actor.context().actorId());
        String reason = ChannelRules.validate(channel);
        if (reason == null && party != null) reason = party.changeChannel();
        if (reason != null)
            return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        actor.context()
                .joinSpot(
                        Names.townId(channel),
                        new JoinTownRequest(actor.context().actorId(), channel))
                .defer();
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> enter(Player actor) {
        var party = state.partyFor(actor.context().actorId());
        if (party == null)
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rules.NOT_LEADER))
                    .submit();
        var reason = party.enter(actor.context().actorId());
        if (reason != null)
            return actor.context().boundSession().send(new ActionRejected(reason)).submit();
        if (party.phase == RunPhase.Active) return show(actor);
        String runId = party.runId;
        String id = Names.dungeonId(runId);
        return spots.getOrCreate(id, Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        result -> {
                            if (result.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Dungeon creation rejected");
                            return context.outbound()
                                    .requestToSpot(
                                            id,
                                            new PrepareDungeon(
                                                    runId,
                                                    party.partyId,
                                                    context.spotId(),
                                                    List.copyOf(party.members)))
                                    .submit(DungeonPrepared.class);
                        })
                .thenCompose(
                        prepared -> {
                            if (prepared.reason() != null
                                    || !id.equals(prepared.dungeonId())
                                    || party.prepared(prepared.runId()) != null)
                                return actor.context()
                                        .boundSession()
                                        .send(new ActionRejected(Rules.RUN_MISMATCH))
                                        .submit();
                            CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
                            var travel = new JoinDungeon(id, runId);
                            for (var member : party.members) {
                                if (!state.players.contains(member)) continue;
                                sent =
                                        sent.thenCompose(
                                                ignored ->
                                                        actors.sendToActor(member, travel)
                                                                .submit());
                            }
                            return sent;
                        });
    }

    public CompletionStage<Void> runStarted(RunStarted message) {
        var party = state.partyForRun(message.runId());
        if (party == null || party.started(message.runId()) != null)
            return rejectRunMismatch(party);
        return broadcast();
    }

    public CompletionStage<Void> runCompleted(RunCompleted message) {
        var party = state.partyForRun(message.runId());
        if (party == null || party.completed(message.runId()) != null)
            return rejectRunMismatch(party);
        return broadcast();
    }

    private CompletionStage<Void> rejectRunMismatch(PartyRun party) {
        if (party == null || party.members.isEmpty()) return CompletableFuture.completedFuture(null);
        return actors.sendToActor(
                        party.members.getFirst(), new ActionRejected(Rules.RUN_MISMATCH))
                .submit();
    }
}
