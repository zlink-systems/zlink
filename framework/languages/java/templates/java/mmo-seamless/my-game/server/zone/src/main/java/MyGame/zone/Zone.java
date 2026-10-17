package MyGame.zone;

import MyGame.*;
import MyGame.border.BorderRoutes;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.*;

public final class Zone implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final List<String> borderTopics;
    ZoneState state = new ZoneState();

    public Zone(ZLinkSpotContext context, ZLinkActorClient actors) {
        this.context = context;
        this.actors = actors;
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
        return context.addTimer(
                        Names.BORDER_TIMER,
                        Duration.ofMillis(Rules.TICK_MILLIS),
                        BorderTick.class,
                        new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                .thenApply(timer -> null);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var arrival = request.decode(EnterZone.class);
        if (!Rules.zoneId(arrival.x()).equals(context.spotId()))
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        if (state.arrivals.putIfAbsent(actorId, arrival) != null)
            throw new IllegalStateException("Duplicate Zone arrival");
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var arrival = state.arrivals.remove(actor.context().actorId());
        if (arrival == null)
            throw new IllegalStateException("Zone admission has no movement intent");
        actor.apply(arrival);
        remember(actor);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        state.visiblePlayers.remove(actor.context().actorId());
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return onLeaveActor(actor);
    }

    public void remember(Player actor) {
        state.visiblePlayers.put(actor.context().actorId(), actor.position());
    }

    public CompletionStage<Void> deliver(Player actor, BorderVisible message) {
        // Membership or visibility may change before a queued notification is handled.
        if (!message.observerZoneId().equals(actor.context().spotId().orElse(null))
                || !state.visiblePlayers.containsKey(actor.context().actorId()))
            return CompletableFuture.completedFuture(null);
        return actor.context().boundSession().send(message).submit();
    }

    public CompletionStage<Void> publishBorder() {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var entry : state.visiblePlayers.entrySet()) {
            var position = entry.getValue();
            int offset = position.x() % Rules.ZONE_WIDTH;
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.ZONE_WIDTH - Rules.BORDER_RADIUS)
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
                            presence.sourceZoneId(),
                            presence.actorId(),
                            presence.x(),
                            presence.sequence());
            var id = observer.getKey();
            sends = sends.thenCompose(ignored -> actors.sendToActor(id, event).submit());
        }
        return sends;
    }
}
