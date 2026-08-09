package systems.zlink.samples.zoneworld.server.zone;

import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class ZoneBootstrap implements ApplicationRunner {
    private final SampleTopology topology;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final NodeMaintenanceState maintenance;
    private final MaintenanceStore store;

    public ZoneBootstrap(
        SampleTopology topology,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        NodeMaintenanceState maintenance,
        MaintenanceStore store) {
        this.topology = topology;
        this.spots = spots;
        this.actors = actors;
        this.maintenance = maintenance;
        this.store = store;
    }

    @Override
    public void run(ApplicationArguments args) {
        maintenance.apply(topology.nodeId(), store.get(topology.nodeId()));
        for (String zone : ZoneWorldSpec.zonesOf(topology.nodeId())) {
            spots.getOrCreate(zone, ZoneWorldNames.ZONE_SPOT_TYPE)
                .inMesh(ZoneWorldNames.MESH)
                .submit()
                .toCompletableFuture()
                .join();
        }
        if ("zone-node-1".equals(topology.nodeId())) {
            for (ZoneWorldSpec.BotFixture bot : ZoneWorldSpec.bots()) {
                actors.getOrCreate(bot.id(), ZoneWorldNames.PLAYER_ACTOR_TYPE)
                    .inMesh(ZoneWorldNames.MESH)
                    .request(new Messages.EnterWorldReq(
                        bot.x(), bot.y(), true, bot.dirX(), bot.dirY()))
                    .submit()
                    .toCompletableFuture()
                    .join();
            }
            System.out.println("bots=8 admitted");
        }
        System.out.println("topology=ready node=" + topology.nodeId()
            + " zones=" + ZoneWorldSpec.zonesOf(topology.nodeId()));
    }
}
