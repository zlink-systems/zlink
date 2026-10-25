package MyGame.zone;

import MyGame.*;
import MyGame.border.BorderRoutes;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.List;
import java.util.concurrent.*;

public final class Zone implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final NodeConfig config;
    private final List<String> borderTopics;
    private final ZonePositionDomain positions;

    public Zone(ZLinkSpotContext context, ZLinkActorClient actors, NodeConfig config) {
        this.context = context;
        this.actors = actors;
        this.config = config;
        positions = new ZonePositionDomain(context.spotId());
        borderTopics =
                Rules.adjacentZones(context.spotId()).stream()
                        .map(target -> Rules.borderTopic(context.spotId(), target))
                        .toList();
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {
        for (var source : Rules.adjacentZones(context.spotId()))
            BorderRoutes.register(context.handlers(), source, context.spotId());
    }

    public CompletionStage<Void> onInitialize() {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var arrival = request.decode(EnterZone.class);
        var rejection = positions.queueArrival(actorId, arrival);
        if (rejection != null)
            return CompletableFuture.completedFuture(
                    ZLinkSpotActorJoinResult.reject(ZLinkMessage.of(rejection)));
        return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept(
                        ZLinkMessage.of(new WorldState(
                                context.spotId(),
                                config.id(),
                                arrival.position().x(),
                                arrival.sequence()))));
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var actorId = actor.context().actorId();
        var arrival = positions.takeArrival(actorId);
        if (arrival == null)
            throw new IllegalStateException("Zone admission has no movement intent");
        positions.addPlayer(actorId, arrival);
        return publishLocal(actorId, new PlayerAppeared(actorId, arrival.position()))
                .thenCompose(ignored -> publishBorder(actorId, arrival.position(), arrival.sequence(),
                        BorderPresenceKind.APPEARED, null));
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        var actorId = actor.context().actorId();
        var state = positions.removePlayer(actorId);
        if (state == null) return CompletableFuture.completedFuture(null);
        return publishLeft(actorId, state.position(), state.sequence(), PlayerLeftReason.LEFT_ZONE);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        var actorId = actor.context().actorId();
        var before = positions.state(actorId);
        if (before == null || !before.connected()) return CompletableFuture.completedFuture(null);
        var state = positions.setConnected(actorId, false);
        return publishLeft(actorId, state.position(), state.sequence(), PlayerLeftReason.DISCONNECTED);
    }

    public CompletionStage<Void> reconnect(Player actor) {
        var actorId = actor.context().actorId();
        var before = positions.state(actorId);
        if (before == null)
            return actor.context().boundSession().send(new ActionRejected("outside-world", 0)).submit();
        var state = positions.setConnected(actorId, true);
        CompletionStage<Void> visible = CompletableFuture.completedFuture(null);
        if (!before.connected())
            visible = publishLocal(actorId, new PlayerAppeared(actorId, state.position()))
                    .thenCompose(ignored -> publishBorder(actorId, state.position(), state.sequence(),
                            BorderPresenceKind.APPEARED, null));
        return visible.thenCompose(ignored -> sendWorldState(actor));
    }

    public CompletionStage<Void> move(Player actor, MoveRequest request) {
        var actorId = actor.context().actorId();
        var movement = positions.move(actorId, request);
        if (!movement.accepted())
            return actor.context().boundSession().send(movement.rejection()).submit();
        if (movement.relocates()) {
            actor.move(
                    movement.zoneId(),
                    new EnterZone(movement.position(), movement.sequence(), movement.remainingMs()));
            return CompletableFuture.completedFuture(null);
        }
        return publishLocal(
                        actorId,
                        new PlayerMoved(actorId, movement.position(), movement.sequence()))
                .thenCompose(ignored -> publishBorder(actorId, movement.position(), movement.sequence(),
                        BorderPresenceKind.MOVED, null))
                .thenCompose(ignored -> sendWorldState(actor));
    }

    public CompletionStage<Void> deliverAppeared(
            Player actor, ZLinkMessageContext dispatch, PlayerAppeared message) {
        return actor.context().boundSession().send(message).submit();
    }

    public CompletionStage<Void> deliverMoved(
            Player actor, ZLinkMessageContext dispatch, PlayerMoved message) {
        return actor.context().boundSession().send(message).submit();
    }

    public CompletionStage<Void> deliverLeft(
            Player actor, ZLinkMessageContext dispatch, PlayerLeft message) {
        return actor.context().boundSession().send(message).submit();
    }

    public CompletionStage<Void> showBorder(BorderPresence presence) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var entry : positions.connectedPlayers().entrySet()) {
            var observerId = entry.getKey();
            var observer = entry.getValue();
            if (observerId.equals(presence.playerId())
                    || Math.abs(observer.position().x() - presence.position().x()) > Rules.BORDER_RADIUS)
                continue;
            Object event = switch (presence.kind()) {
                case APPEARED -> new PlayerAppeared(presence.playerId(), presence.position());
                case MOVED -> new PlayerMoved(presence.playerId(), presence.position(), presence.sequence());
                case LEFT -> new PlayerLeft(
                        presence.playerId(),
                        presence.reason() == null ? PlayerLeftReason.LEFT_ZONE : presence.reason());
            };
            sends = sends.thenCompose(ignored -> actors.sendToActor(observerId, event).submit());
        }
        return sends;
    }

    public ZonePositionDomain.ZonePositionSnapshot capture() {
        return positions.capture();
    }

    public void restore(ZonePositionDomain.ZonePositionSnapshot snapshot) {
        positions.restore(snapshot);
    }

    private CompletionStage<Void> publishLocal(String playerId, Object event) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var observerId : positions.connectedPlayers().keySet()) {
            if (observerId.equals(playerId)) continue;
            sends = sends.thenCompose(ignored -> actors.sendToActor(observerId, event).submit());
        }
        return sends;
    }

    private CompletionStage<Void> publishLeft(
            String playerId, Position position, long sequence, PlayerLeftReason reason) {
        return publishLocal(playerId, new PlayerLeft(playerId, reason))
                .thenCompose(ignored -> publishBorder(
                        playerId, position, sequence, BorderPresenceKind.LEFT, reason));
    }

    private CompletionStage<Void> publishBorder(
            String playerId,
            Position position,
            long sequence,
            BorderPresenceKind kind,
            PlayerLeftReason reason) {
        double offset = position.x() % Rules.ZONE_WIDTH;
        if (offset >= Rules.BORDER_RADIUS && offset < Rules.ZONE_WIDTH - Rules.BORDER_RADIUS)
            return CompletableFuture.completedFuture(null);
        var presence = new BorderPresence(context.spotId(), playerId, position, sequence, kind, reason);
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var topic : borderTopics)
            sends = sends.thenCompose(ignored -> context.outbound()
                    .publish(Names.BORDER, topic, presence)
                    .submit());
        return sends;
    }

    private CompletionStage<Void> sendWorldState(Player actor) {
        var state = positions.state(actor.context().actorId());
        return actor.context().boundSession().send(new WorldState(
                context.spotId(), config.id(), state.position().x(), state.sequence())).submit();
    }
}
