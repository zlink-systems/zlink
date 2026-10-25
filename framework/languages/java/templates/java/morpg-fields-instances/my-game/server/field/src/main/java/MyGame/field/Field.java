package MyGame.field;

import MyGame.*;
import MyGame.border.BorderRoutes;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;

public final class Field implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final List<String> borderTopics;
    private final ZLinkSpotManager spots;
    FieldState state = new FieldState();
    private final FieldPositionDomain positions;

    public Field(
            ZLinkSpotContext context,
            ZLinkActorClient actors,
            ZLinkSpotManager spots) {
        this.spots = spots;
        this.context = context;
        this.actors = actors;
        this.positions = new FieldPositionDomain(context.spotId());
        borderTopics =
                Rules.adjacentFields(context.spotId()).stream()
                        .map(target -> Rules.borderTopic(context.spotId(), target))
                        .toList();
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {
        for (var source : Rules.adjacentFields(context.spotId()))
            BorderRoutes.register(context.handlers(), source, context.spotId());
    }

    public CompletionStage<Void> onInitialize() {
        return context.addTimer(
                        Names.BORDER_TIMER,
                        Duration.ofMillis(Rules.TICK_MILLIS),
                        BorderTick.class,
                        new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                .thenApply(timer -> null);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var rejection = positions.queueArrival(actorId, request.decode(EnterField.class));
        return CompletableFuture.completedFuture(
                rejection == null
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var arrival = positions.takeArrival(actor.context().actorId());
        if (arrival == null)
            throw new IllegalStateException("Field admission has no movement intent");
        positions.addPlayer(actor.context().actorId(), arrival);
        actor.apply(arrival);
        remember(actor);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        state.visiblePlayers.remove(actor.context().actorId());
        positions.removePlayer(actor.context().actorId());
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        state.visiblePlayers.remove(actor.context().actorId());
        positions.setConnected(actor.context().actorId(), false);
        return CompletableFuture.completedFuture(null);
    }

    public void remember(Player actor) {
        positions.setConnected(actor.context().actorId(), true);
        state.visiblePlayers.put(actor.context().actorId(), actor.position());
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var result = positions.move(actor.context().actorId(), request);
        if (result.rejection() != null)
            return actor.context().boundSession().send(result.rejection()).submit();
        var arrival = new EnterField(result.position().x(), result.sequence(), result.remainingMs());
        if (result.relocates()) {
            actor.move(arrival);
            return CompletableFuture.completedFuture(null);
        }
        actor.apply(arrival);
        remember(actor);
        return actor.sendState();
    }

    public CompletionStage<Void> publishBorder() {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var entry : state.visiblePlayers.entrySet()) {
            var position = entry.getValue();
            double offset = position.x() % Rules.FIELD_WIDTH;
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.FIELD_WIDTH - Rules.BORDER_RADIUS)
                continue;
            var presence =
                    new BorderPresence(
                            context.spotId(), entry.getKey(), position.x(), position.sequence());
            for (var topic : borderTopics)
                sends =
                        sends.thenCompose(
                                ignored ->
                                        context.outbound()
                                                .publish(Names.BORDER, topic, presence)
                                                .submit());
        }
        return sends;
    }

    public CompletionStage<Void> showBorder(BorderPresence presence) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var observer : state.visiblePlayers.entrySet()) {
            if (Math.abs(observer.getValue().x() - presence.x()) > Rules.BORDER_RADIUS) continue;
            var event =
                    new BorderVisible(
                            context.spotId(),
                            presence.sourceFieldId(),
                            presence.playerId(),
                            presence.x(),
                            presence.sequence());
            var id = observer.getKey();
            sends = sends.thenCompose(ignored -> actors.sendToActor(id, event).submit());
        }
        return sends;
    }

    public CompletionStage<Void> party(Player actor, String partyId, boolean create) {
        if (actor.partyId() != null) return reject(actor, Rejection.INVALID_PARTY);
        String partySpotId = create ? "party-" + UUID.randomUUID().toString().replace("-", "") : partyId;
        if (!PartyRun.validId(partySpotId, "party-")) return reject(actor, Rejection.INVALID_PARTY);
        if (create)
            return spots.getOrCreate(partySpotId, Names.PARTY)
                    .inMesh(Names.MESH)
                    .submit()
                    .thenCompose(
                            created -> {
                                if (created.state() == ZLinkSpotCreateState.REJECTED)
                                    return CompletableFuture.failedFuture(
                                            new IllegalStateException("Party creation rejected"));
                                return joinParty(actor, partySpotId);
                            });
        return spots.find(partySpotId)
                .thenCompose(
                        found ->
                                found.isPresent()
                                        ? joinParty(actor, partySpotId)
                                        : reject(actor, Rejection.INVALID_PARTY));
    }

    public CompletionStage<Void> enter(Player actor) {
        if (actor.partyId() == null) return reject(actor, Rejection.NOT_LEADER);
        return context.outbound()
                .requestToSpot(
                        actor.partyId(), new PartyCommand(actor.context().actorId(), false, true))
                .submit(PartyCommandResult.class)
                .thenCompose(
                        result ->
                                result.rejection() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : actor.context().boundSession().send(result.rejection()).submit());
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.sendState()
                .thenCompose(
                        ignored -> {
                            if (actor.partyId() == null) return CompletableFuture.completedFuture(null);
                            return context.outbound()
                                    .requestToSpot(actor.partyId(), new PartyCommand(actor.context().actorId()))
                                    .submit(PartyCommandResult.class)
                                    .thenCompose(
                                            result -> result.view() == null
                                                    ? CompletableFuture.completedFuture(null)
                                                    : actor.context().boundSession().send(result.view()).submit());
                        });
    }

    private CompletionStage<Void> joinParty(Player actor, String partyId) {
        return context.outbound()
                .requestToSpot(partyId, new PartyCommand(actor.context().actorId(), true, false))
                .submit(PartyCommandResult.class)
                .thenCompose(
                        result -> {
                            if (result.rejection() != null)
                                return actor.context().boundSession().send(result.rejection()).submit();
                            actor.setPartyId(partyId);
                            return CompletableFuture.completedFuture(null);
                        });
    }

    private CompletionStage<Void> reject(Player actor, String reason) {
        return actor.context().boundSession().send(new ActionRejected(reason)).submit();
    }

    public FieldSnapshot capture() {
        return new FieldSnapshot(new java.util.LinkedHashMap<>(state.visiblePlayers), positions.capture());
    }

    public void restore(FieldSnapshot snapshot) {
        positions.restore(snapshot.positions());
        state.visiblePlayers.clear();
        state.visiblePlayers.putAll(snapshot.visiblePlayers());
    }

    public record FieldSnapshot(
            java.util.Map<String, EnterField> visiblePlayers,
            FieldPositionDomain.FieldPositionSnapshot positions) {}
}
