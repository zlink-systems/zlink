package systems.zlink.samples.zoneworld.server.zone;

import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.springframework.context.SmartLifecycle;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;

public final class ZoneStatusReporter implements SmartLifecycle, AutoCloseable {
    private final SampleTopology topology;
    private final ZLinkRouteClient routes;
    private final NodeCensus census;
    private final NodeMaintenanceState maintenance;
    private final Object lifecycleLock = new Object();
    private ScheduledExecutorService scheduler;
    private boolean running;

    public ZoneStatusReporter(
        SampleTopology topology,
        ZLinkRouteClient routes,
        NodeCensus census,
        NodeMaintenanceState maintenance) {
        this.topology = topology;
        this.routes = routes;
        this.census = census;
        this.maintenance = maintenance;
    }

    @Override
    public void start() {
        synchronized (lifecycleLock) {
            if (topology.isSubscriberOnly()) return;
            if (running) {
                return;
            }
            scheduler = Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable, "zoneworld-status-" + topology.nodeId());
                thread.setDaemon(true);
                return thread;
            });
            running = true;
            scheduler.scheduleAtFixedRate(this::report, 0,
                ZoneWorldSpec.NODE_STATUS_REPORT_PERIOD_MS, TimeUnit.MILLISECONDS);
        }
    }

    private void report() {
        synchronized (lifecycleLock) {
            if (!running) {
                return;
            }
            List<String> zones = census.zoneIds();
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
                        } else {
                            System.out.println("node status report submitted. node="
                                + topology.nodeId());
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
    }

    @Override
    public void stop() {
        synchronized (lifecycleLock) {
            running = false;
            if (scheduler != null) {
                scheduler.shutdownNow();
                scheduler = null;
            }
        }
    }

    @Override
    public void stop(Runnable callback) {
        try {
            stop();
        } finally {
            callback.run();
        }
    }

    @Override
    public boolean isRunning() {
        synchronized (lifecycleLock) {
            return running;
        }
    }

    @Override
    public int getPhase() {
        return 1;
    }

    @Override
    public void close() {
        stop();
    }
}
