package systems.zlink.samples.zoneworld.server.zone;

import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class ZoneStatusReporter implements AutoCloseable {
    private final SampleTopology topology;
    private final ZLinkRouteClient routes;
    private final NodeCensus census;
    private final NodeMaintenanceState maintenance;
    private final ScheduledExecutorService scheduler;

    public ZoneStatusReporter(
        SampleTopology topology,
        ZLinkRouteClient routes,
        NodeCensus census,
        NodeMaintenanceState maintenance) {
        this.topology = topology;
        this.routes = routes;
        this.census = census;
        this.maintenance = maintenance;
        scheduler = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "zoneworld-status-" + topology.nodeId());
            thread.setDaemon(true);
            return thread;
        });
        scheduler.scheduleAtFixedRate(this::report, 0, 1, TimeUnit.SECONDS);
    }

    private void report() {
        List<String> zones = ZoneWorldSpec.zonesOf(topology.nodeId());
        try {
            routes.sendToChannel(
                    ZoneWorldNames.REPORT_CHANNEL,
                    new Messages.ReportNodeStatusMsg(
                        topology.nodeId(),
                        zones,
                        census.total(),
                        maintenance.isUnderMaintenance(topology.nodeId())))
                .submit()
                .whenComplete((ignored, error) -> {
                    if (error != null) {
                        System.out.println("report failed node=" + topology.nodeId()
                            + " detail=" + error.getMessage());
                    }
                });
        } catch (RuntimeException error) {
            // A fixed-rate task is cancelled when an invocation escapes with an
            // exception. Ops can start after a Zone node, so keep the periodic
            // public-channel retry alive across that expected readiness window.
            System.out.println("report failed node=" + topology.nodeId()
                + " detail=" + error.getMessage());
        }
    }

    @Override
    public void close() {
        scheduler.shutdownNow();
    }
}
