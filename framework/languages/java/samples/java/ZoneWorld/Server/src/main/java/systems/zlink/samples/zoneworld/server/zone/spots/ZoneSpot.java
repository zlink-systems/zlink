package systems.zlink.samples.zoneworld.server.zone.spots;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
/** The authoritative state holder for one logical Zone Spot. */
public final class ZoneSpot implements ZLinkSpot<PlayerActor> {
    private final ZLinkSpotContext context;
    private final NodeMaintenanceState maintenance;
    private final NodeCensus census;
    private final ZLinkActorClient actorClient;
    private final SampleTopology topology;
    private final Map<String, PlayerActor> residents = new HashMap<>();
    private final Map<String, Messages.EnterZoneReq> pendingJoins = new HashMap<>();
    private final Map<String, BorderSnapshot> borderSnapshots = new HashMap<>();
    private ZLinkTimer tickTimer;
    private ZLinkTimer botTimer;
    private long tick;

    public ZoneSpot(
        ZLinkSpotContext context,
        NodeMaintenanceState maintenance,
        NodeCensus census,
        ZLinkActorClient actorClient,
        SampleTopology topology) {
        this.context = context;
        this.maintenance = maintenance;
        this.census = census;
        this.actorClient = actorClient;
        this.topology = topology;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        Messages.EnterZoneReq join = request.decode(Messages.EnterZoneReq.class);
        String zone = context.spotId();
        if (!actorId.equals(join.playerId()) || !zone.equals(ZoneWorldSpec.zoneOf(join.x(), join.y()))) {
            return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.reject(
                    new Messages.EnterZoneRes(zone, "InvalidZone")));
        }
        if (maintenance.rejectsArrival(topology.nodeId(), zone, join.fromZoneId())) {
            return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.reject(
                    new Messages.EnterZoneRes(zone, "ZoneMaintenance")));
        }
        if (join.crashBoundaryProbe()) {
            System.out.println("crash-boundary join pending zone=" + zone
                + " actor=" + actorId);
            return new CompletableFuture<>();
        }
        pendingJoins.put(actorId, join);
        return CompletableFuture.completedFuture(
            ZLinkSpotActorJoinResult.accept(new Messages.EnterZoneRes(zone, null)));
    }

    @Override
    public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
        Messages.EnterZoneReq join = pendingJoins.remove(actor.actorId());
        if (join == null) {
            return CompletableFuture.completedFuture(null);
        }
        actor.applyAtZone(join.x(), join.y(), context.spotId(), join.isBot());
        residents.put(actor.actorId(), actor);
        census.record(context.spotId(), residents.size());
        System.out.println("zone actor joined zone=" + context.spotId()
            + " actor=" + actor.actorId() + " generation=" + actor.context().objectGeneration()
            + " player=" + actor.actorId() + ", bot=" + actor.isBot()
            + ", initial=" + join.initialEntry());
        if (actor.isBot()) return CompletableFuture.completedFuture(null);
        if (!join.initialEntry()) {
            return actor.send(new Messages.ZoneChangedNotify(
                actor.actorId(), context.spotId()));
        }
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
        residents.remove(actor.actorId(), actor);
        census.record(context.spotId(), residents.size());
        System.out.println("zone actor left zone=" + context.spotId() + " actor=" + actor.actorId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(PlayerActor actor) {
        residents.remove(actor.actorId(), actor);
        census.record(context.spotId(), residents.size());
        System.out.println("zone actor disconnected zone=" + context.spotId()
            + " actor=" + actor.actorId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        census.hostZone(context.spotId());
        CompletionStage<ZLinkTimer> tick = context.addTimer(
            "zone-tick", Duration.ofMillis(ZoneWorldSpec.TICK_PERIOD_MS), ZoneTickHandler.class, null);
        CompletionStage<ZLinkTimer> bots = context.addTimer(
            "zone-bot-tick", Duration.ofMillis(ZoneWorldSpec.BOT_TICK_PERIOD_MS),
            ZoneBotTickHandler.class, null);
        return tick.thenCombine(bots, (first, second) -> {
            tickTimer = first;
            botTimer = second;
            return (Void) null;
        });
    }

    @Override
    public CompletionStage<Void> onClosing() {
        census.releaseZone(context.spotId());
        CompletionStage<Void> first = tickTimer == null
            ? CompletableFuture.completedFuture(null)
            : tickTimer.cancel();
        CompletionStage<Void> second = botTimer == null
            ? CompletableFuture.completedFuture(null)
            : botTimer.cancel();
        return first.thenCombine(second, (ignored, alsoIgnored) -> (Void) null);
    }

    public CompletionStage<Void> tick() {
        tick++;
        borderSnapshots.entrySet().removeIf(entry -> tick - entry.getValue().tick()
            > ZoneWorldSpec.BORDER_EXPIRY_TICKS);
        publishBorders();
        List<Messages.PlayerView> players = statePlayers();
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (PlayerActor actor : List.copyOf(residents.values())) {
            if (!actor.isBot()) {
                sends = sends.thenCompose(ignored -> actorClient.sendToActor(
                        actor.actorId(),
                        new Messages.DeliverZoneStateMsg(context.spotId(), tick, players))
                    .submit());
            }
        }
        return sends.exceptionally(error -> {
            System.out.println("zone tick delivery error zone=" + context.spotId()
                + " detail=" + error.getMessage());
            return null;
        });
    }

    public CompletionStage<Void> botTick() {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (PlayerActor actor : List.copyOf(residents.values())) {
            if (actor.isBot() && !actor.pendingJoin()) {
                sends = sends.thenCompose(ignored -> actorClient.sendToActor(
                        actor.actorId(), new Messages.BotTickMsg()).submit());
            }
        }
        return sends;
    }

    public CompletionStage<Void> move(PlayerActor actor, int targetX, int targetY) {
        ZoneWorldSpec.MoveDecision decision = ZoneWorldSpec.validateMove(
            actor.x(), actor.y(), targetX, targetY);
        if (!decision.accepted()) {
            if (!actor.isBot()) {
                return actor.send(new Messages.MoveRejectedNotify(
                    decision.reason(), actor.x(), actor.y()));
            }
            return CompletableFuture.completedFuture(null);
        }
        String targetZone = ZoneWorldSpec.zoneOf(targetX, targetY);
        if (!decision.zoneChanged()) {
            actor.updatePosition(targetX, targetY);
            census.record(context.spotId(), residents.size());
            context.outbound().sendToSpot(context.spotId(),
                new Messages.UpdatePositionMsg(actor.actorId(), targetX, targetY, actor.isBot()))
                .submit();
            if (actor.isBot()) return CompletableFuture.completedFuture(null);
            return actor.send(new Messages.ZoneStateNotify(
                context.spotId(), tick, statePlayers()));
        }

        actor.prepareMove(targetX, targetY, targetZone);
        actor.context().joinSpot(
                targetZone,
                new Messages.EnterZoneReq(
                    actor.actorId(),
                    targetX,
                    targetY,
                    actor.isBot(),
                    false,
                    actor.zoneId(),
                    false))
            .timeout(Duration.ofSeconds(10))
            .defer();
        System.out.println("zone transfer requested actor=" + actor.actorId()
            + " from=" + context.spotId() + " to=" + targetZone
            + " node=" + topology.nodeId());
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> crashProbe(PlayerActor actor, int targetX, int targetY) {
        ZoneWorldSpec.MoveDecision decision = ZoneWorldSpec.validateMove(
            actor.x(), actor.y(), targetX, targetY);
        if (!decision.accepted() || !decision.zoneChanged()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException("Crash probe requires one legal cross-zone move"));
        }
        String targetZone = ZoneWorldSpec.zoneOf(targetX, targetY);
        actor.prepareCrashProbe(targetX, targetY, targetZone);
        actor.context().joinSpot(targetZone, new Messages.EnterZoneReq(
                actor.actorId(), targetX, targetY, actor.isBot(), false,
                actor.zoneId(), true))
            .timeout(Duration.ofSeconds(30)).defer();
        return CompletableFuture.completedFuture(null);
    }

    public void applyPosition(Messages.UpdatePositionMsg update) {
        PlayerActor actor = residents.get(update.playerId());
        if (actor != null) {
            actor.updatePosition(update.x(), update.y());
        }
    }

    public CompletionStage<Void> deliverState(
        PlayerActor actor,
        Messages.DeliverZoneStateMsg message) {
        if (residents.get(actor.actorId()) != actor) {
            return CompletableFuture.completedFuture(null);
        }
        return actor.send(new Messages.ZoneStateNotify(
            message.zoneId(), message.tick(), message.players()));
    }

    public void applyBorder(Messages.ZoneBorderEvent event) {
        if (!context.spotId().equals(event.toZoneId())) return;
        BorderSnapshot current = borderSnapshots.get(event.fromZoneId());
        if (current == null || event.tick() >= current.tick()) {
            borderSnapshots.put(event.fromZoneId(),
                new BorderSnapshot(event.tick(), List.copyOf(event.players())));
        }
    }

    public void deliverAnnouncement(Messages.DeliverAnnounceMsg message) {
        System.out.println("zone spot: announcement delivered zone=" + context.spotId()
            + " id=" + message.announcementId());
        for (PlayerActor actor : List.copyOf(residents.values())) {
            if (!actor.isBot()) actor.send(new Messages.WorldAnnounceNotify(
                message.announcementId(), message.text()));
        }
    }

    public int playerCount() {
        return residents.size();
    }

    public List<Messages.PlayerView> statePlayers() {
        Map<String, Messages.PlayerView> players = new HashMap<>();
        for (PlayerActor actor : residents.values()) {
            players.put(actor.actorId(), new Messages.PlayerView(
                actor.actorId(), actor.x(), actor.y(), context.spotId(), actor.isBot()));
        }
        for (BorderSnapshot snapshot : borderSnapshots.values()) {
            for (Messages.PlayerView player : snapshot.players()) {
                players.putIfAbsent(player.playerId(), player);
            }
        }
        return players.values().stream()
            .sorted(Comparator.comparing(Messages.PlayerView::playerId, ZoneWorldSpec.UTF8_ORDER))
            .toList();
    }

    private void publishBorders() {
        for (String target : ZoneWorldSpec.adjacentZones(context.spotId())) {
            List<Messages.PlayerView> border = residents.values().stream()
                .filter(actor -> ZoneWorldSpec.inBorderBand(
                    actor.x(), actor.y(), context.spotId(), target))
                .map(actor -> new Messages.PlayerView(
                    actor.actorId(), actor.x(), actor.y(), context.spotId(), actor.isBot()))
                .sorted(Comparator.comparing(Messages.PlayerView::playerId, ZoneWorldSpec.UTF8_ORDER))
                .toList();
            context.outbound().publish(
                    ZoneWorldNames.ZONE_CHANNEL,
                    ZoneWorldNames.borderTopic(context.spotId(), target),
                    new Messages.ZoneBorderEvent(context.spotId(), target, tick, border))
                .submit();
        }
    }

    private record BorderSnapshot(long tick, List<Messages.PlayerView> players) {
    }
}
