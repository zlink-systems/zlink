package systems.zlink.samples.zoneworld.server.zone;

import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

public final class ZoneBootstrap implements ApplicationRunner {
    private static final int STARTUP_RETRY_ATTEMPTS = 120;
    private static final int REPLACEMENT_READY_ATTEMPTS = 8;
    private static final int STARTUP_RETRY_DELAY_MS = 250;
    private final SampleTopology topology;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;
    private final NodeMaintenanceState maintenance;
    private final MaintenanceStore store;
    private final NodeCensus census;
    private final ZoneStatusReporter reporter;

    public ZoneBootstrap(
            SampleTopology topology,
            ZLinkSpotManager spots,
            ZLinkActorManager actors,
            ZLinkActorClient actorClient,
            NodeMaintenanceState maintenance,
            MaintenanceStore store,
            NodeCensus census,
            ZoneStatusReporter reporter) {
        this.topology = topology;
        this.spots = spots;
        this.actors = actors;
        this.actorClient = actorClient;
        this.maintenance = maintenance;
        this.store = store;
        this.census = census;
        this.reporter = reporter;
    }

    // Ops learns a node's zone set only from the node's own status report (README §2.2). The
    // report is sent once the zone set is settled and before topology=ready is printed, so the
    // report an observer gates on never carries the pre-claim census; the periodic report and
    // the maintenance-change report are the only other senders.
    private void ready() {
        reporter.reportNow().exceptionally(error -> null).toCompletableFuture().join();
        System.out.println(
                "topology=ready node="
                        + topology.nodeId()
                        + " zones="
                        + String.join(",", census.zoneIds()));
    }

    @Override
    public void run(ApplicationArguments args) {
        if (topology.isSubscriberOnly()) {
            System.out.println("topology=ready node=" + topology.nodeId() + " zones=");
            return;
        }
        // Maintenance is desired state, not a message: a node that starts reads it back from
        // the store, so a restart cannot quietly reopen a node the operator closed.
        boolean restored = store.get(topology.nodeId());
        maintenance.apply(topology.nodeId(), restored);
        System.out.println(
                "maintenance restored node=" + topology.nodeId() + " enabled=" + restored);
        for (String nodeId : java.util.List.of("zone-node-1", "zone-node-2")) {
            maintenance.apply(nodeId, store.get(nodeId));
        }
        // A replacement keeps the NodeId and claims nothing. A stopped owner's zone objects stay
        // with the incarnation that owned them, so claiming here could settle on one zone — which
        // violates the empty zone set a replacement announces, and is a state
        // the loop below could never leave. Only a cold start claims.
        if (topology.allowsEmptyZoneSet()) {
            for (int attempt = 0; attempt < REPLACEMENT_READY_ATTEMPTS; attempt++)
                CompletableFuture.runAsync(
                                () -> {},
                                CompletableFuture.delayedExecutor(
                                        STARTUP_RETRY_DELAY_MS, TimeUnit.MILLISECONDS))
                        .join();
            if (!census.zoneIds().isEmpty())
                throw new IllegalStateException("A replacement must reach ready with no Zone");
            ready();
            return;
        }
        for (int attempt = 0; census.zoneIds().size() != topology.zoneCapacityValue(); attempt++) {
            java.util.List<String> claimed = census.zoneIds();
            for (String zone : ZoneWorldSpec.zones()) {
                if (!census.zoneIds().equals(claimed)) break;
                try {
                    spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                            .inMesh(ZoneWorldNames.MESH)
                            .submit()
                            .toCompletableFuture()
                            .join();
                } catch (java.util.concurrent.CompletionException error) {
                    if (!(error.getCause() instanceof ZLinkFrameworkException failure)
                            || (failure.kind() != ZLinkFrameworkErrorKind.UNAVAILABLE
                                    && failure.kind() != ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED))
                        throw error;
                    System.err.println("Zone Spot claim failed zone=" + zone + " error=" + failure);
                }
            }
            if (attempt + 1 >= STARTUP_RETRY_ATTEMPTS)
                throw new IllegalStateException(
                        "Zone Spot capacity did not settle. node="
                                + topology.nodeId()
                                + " zones="
                                + census.zoneIds());
            CompletableFuture.runAsync(
                            () -> {},
                            CompletableFuture.delayedExecutor(
                                    STARTUP_RETRY_DELAY_MS, TimeUnit.MILLISECONDS))
                    .join();
        }
        if (!topology.botsDisabled()) {
            for (ZoneWorldSpec.BotFixture bot :
                    ZoneWorldSpec.bots().stream()
                            .filter(
                                    value ->
                                            census.zoneIds()
                                                    .contains(
                                                            ZoneWorldSpec.zoneOf(
                                                                    value.x(), value.y())))
                            .toList()) {
                ZLinkActorCreateResult result =
                        actors.getOrCreate(bot.id(), ZoneWorldNames.PLAYER_ACTOR_TYPE)
                                .inMesh(ZoneWorldNames.MESH)
                                .request(ZLinkMessage.empty())
                                .submit()
                                .toCompletableFuture()
                                .join();
                if (result instanceof ZLinkActorCreateResult.Created created) {
                    actorClient
                            .requestToActor(
                                    created.actor().actorId(),
                                    new Messages.EnterWorldReq(
                                            bot.x(), bot.y(), true, bot.dirX(), bot.dirY()))
                            .submit(Messages.EnterWorldRes.class)
                            .toCompletableFuture()
                            .join();
                }
                System.out.println(
                        "bot spawned. bot="
                                + bot.id()
                                + ", zone="
                                + ZoneWorldSpec.zoneOf(bot.x(), bot.y())
                                + ", start=("
                                + bot.x()
                                + ","
                                + bot.y()
                                + ")"
                                + ", dir=("
                                + bot.dirX()
                                + ","
                                + bot.dirY()
                                + ")");
            }
        }
        ready();
    }
}
