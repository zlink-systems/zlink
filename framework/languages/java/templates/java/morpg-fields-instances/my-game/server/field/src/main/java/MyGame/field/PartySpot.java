package MyGame.field;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

// The party owns membership and run state; its ID is also this Spot ID.
public final class PartySpot implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final ZLinkSpotManager spots;
    private PartyRun state = new PartyRun();

    public PartySpot(ZLinkSpotContext context, ZLinkActorClient actors, ZLinkSpotManager spots) {
        this.context = context;
        this.actors = actors;
        this.spots = spots;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {}

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String actorId, ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<PartyCommandResult> command(PartyCommand command) {
        var rejection = command.join()
                ? state.join(command.actorId())
                : command.enter()
                        ? state.enter(command.actorId())
                        : state.members.contains(command.actorId())
                                ? null
                                : new ActionRejected(Rejection.INVALID_PARTY);
        if (rejection != null)
            return CompletableFuture.completedFuture(new PartyCommandResult(null, rejection));
        if (!command.enter() || state.phase == RunPhase.Active)
            return finishCommand(command);

        String runId = state.runId;
        String dungeonId = Rules.dungeonId(runId);
        return spots.getOrCreate(dungeonId, Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("Dungeon creation rejected"));
                            return context.outbound()
                                    .requestToSpot(
                                            dungeonId,
                                            new PrepareDungeon(
                                                    runId,
                                                    context.spotId(),
                                                    List.copyOf(state.members)))
                                    .submit(DungeonPrepared.class);
                        })
                .thenCompose(
                        prepared -> {
                            if (prepared.rejection() != null)
                                return CompletableFuture.completedFuture(
                                        new PartyCommandResult(null, prepared.rejection()));
                            if (!dungeonId.equals(prepared.dungeonId()))
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("Dungeon identity mismatch"));
                            var failure = state.prepared(prepared.runId());
                            if (failure != null)
                                return CompletableFuture.completedFuture(
                                        new PartyCommandResult(null, failure));
                            CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
                            for (var id : state.members) {
                                if (state.entered.contains(id)) continue;
                                sent = sent.thenCompose(
                                        ignored -> actors.sendToActor(id, new JoinDungeon(dungeonId, runId)).submit());
                            }
                            return sent.thenCompose(ignored -> finishCommand(command));
                        });
    }

    public CompletionStage<PartyCommandResult> progress(String runId, String actorId, boolean completed) {
        var rejection = actorId != null
                ? state.arrived(runId, actorId)
                : completed ? state.completed(runId) : state.started(runId);
        if (rejection != null)
            return CompletableFuture.completedFuture(new PartyCommandResult(null, rejection));
        return broadcast().thenApply(ignored -> new PartyCommandResult(state.view(context.spotId()), null));
    }

    private CompletionStage<PartyCommandResult> finishCommand(PartyCommand command) {
        CompletionStage<Void> sent = command.join() || command.enter()
                ? broadcast()
                : CompletableFuture.completedFuture(null);
        return sent.thenApply(ignored -> new PartyCommandResult(state.view(context.spotId()), null));
    }

    private CompletionStage<Void> broadcast() {
        var view = state.view(context.spotId());
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var id : state.members)
            sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }

    public byte[] capture() {
        try {
            return Json.MAPPER.writeValueAsBytes(state);
        } catch (java.io.IOException error) {
            throw new IllegalStateException("Party serialization failed", error);
        }
    }

    public void restore(byte[] payload) {
        try {
            state = Json.MAPPER.readValue(payload, PartyRun.class);
        } catch (java.io.IOException error) {
            throw new IllegalStateException("Party state is invalid", error);
        }
    }
}
