package systems.zlink.samples.zoneworld.server.zone;

import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class ZoneBootstrap implements ApplicationRunner {
    private final SampleTopology topology;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;
    private final NodeMaintenanceState maintenance;
    private final MaintenanceStore store;
    private final NodeCensus census;

    public ZoneBootstrap(
        SampleTopology topology,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        NodeMaintenanceState maintenance,
        MaintenanceStore store,
        NodeCensus census) {
        this.topology = topology;
        this.spots = spots;
        this.actors = actors;
        this.actorClient = actorClient;
        this.maintenance = maintenance;
        this.store = store;
        this.census = census;
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
        System.out.println("maintenance restored node=" + topology.nodeId()
            + " enabled=" + restored);
        for (String nodeId : java.util.List.of("zone-node-1", "zone-node-2")) {
            maintenance.apply(nodeId, store.get(nodeId));
        }
        for (int attempt = 0; census.zoneIds().size() != 2; attempt++) {
            for (String zone : ZoneWorldSpec.zones()) {
                try {
                    spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                        .inMesh(ZoneWorldNames.MESH).submit().toCompletableFuture().join();
                } catch (RuntimeException ignored) {
                    // The other eligible process may still be entering the mesh.
                }
            }
            if (topology.allowsEmptyZoneSet() && census.zoneIds().isEmpty() && attempt >= 8) break;
            if (attempt >= 119) throw new IllegalStateException(
                "Zone Spot capacity did not settle. node=" + topology.nodeId()
                    + " zones=" + census.zoneIds());
            CompletableFuture.runAsync(() -> {},
                CompletableFuture.delayedExecutor(250, TimeUnit.MILLISECONDS)).join();
        }
        if (!topology.botsDisabled()) {
            for (ZoneWorldSpec.BotFixture bot : ZoneWorldSpec.bots().stream()
                    .filter(value -> census.zoneIds().contains(
                        ZoneWorldSpec.zoneOf(value.x(), value.y())))
                    .toList()) {
                ZLinkActorCreateResult result = actors.getOrCreate(
                        bot.id(), ZoneWorldNames.PLAYER_ACTOR_TYPE)
                    .inMesh(ZoneWorldNames.MESH)
                    .request(ZLinkMessage.empty())
                    .submit()
                    .toCompletableFuture()
                    .join();
                if (result instanceof ZLinkActorCreateResult.Created created) {
                    actorClient.requestToActor(created.actor().actorId(),
                            new Messages.EnterWorldReq(
                                bot.x(), bot.y(), true, bot.dirX(), bot.dirY()))
                        .submit(Messages.EnterWorldRes.class)
                        .toCompletableFuture()
                        .join();
                }
                System.out.println("bot spawned. bot=" + bot.id()
                    + ", zone=" + ZoneWorldSpec.zoneOf(bot.x(), bot.y())
                    + ", start=(" + bot.x() + "," + bot.y() + ")"
                    + ", dir=(" + bot.dirX() + "," + bot.dirY() + ")");
            }
        }
        System.out.println("topology=ready node=" + topology.nodeId()
            + " zones=" + census.zoneIds());
    }
}
