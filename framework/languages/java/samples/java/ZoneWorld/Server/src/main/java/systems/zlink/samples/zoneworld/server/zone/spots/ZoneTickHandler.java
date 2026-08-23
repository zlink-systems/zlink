package systems.zlink.samples.zoneworld.server.zone.spots;

import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;

public final class ZoneTickHandler implements ZLinkSpotTimerHandler<ZoneSpot> {
    private static final AtomicBoolean FAULT_INJECTED = new AtomicBoolean();
    private final SampleTopology topology;
    private final ZLinkRouteClient routes;

    public ZoneTickHandler(SampleTopology topology, ZLinkRouteClient routes) {
        this.topology = topology;
        this.routes = routes;
    }

    @Override
    public CompletionStage<Void> handle(ZoneSpot spot, ZLinkTimerTick tick) {
        CompletionStage<Void> operation;
        try {
            String faultZone = topology.faultTickZone();
            if (("*".equals(faultZone) || spot.context().spotId().equals(faultZone))
                && FAULT_INJECTED.compareAndSet(false, true)) {
                throw new IllegalStateException(
                    "injected tick failure for ZW-C4. zone=" + spot.context().spotId());
            }
            operation = spot.tick();
        } catch (RuntimeException error) {
            operation = CompletableFuture.failedFuture(error);
        }
        return operation.exceptionallyCompose(error -> reportAndRethrow(spot, error));
    }

    private CompletionStage<Void> reportAndRethrow(ZoneSpot spot, Throwable error) {
        Messages.ReportSpotEventMsg report = new Messages.ReportSpotEventMsg(
            topology.nodeId(),
            "TimerHandlerFailed",
            "spot=" + spot.context().spotId() + "; timer=zone-tick; detail=" + error.getMessage(),
            Instant.now().toString());
        return routes.sendToChannel(ZoneWorldNames.REPORT_CHANNEL, report).submit()
            .handle((ignored, reportError) -> {
                if (reportError != null) {
                    System.err.println("zone spot event report failed. zone="
                        + spot.context().spotId() + " error=" + reportError.getMessage());
                }
                return (Void) null;
            })
            .thenCompose(ignored -> CompletableFuture.failedFuture(error));
    }
}
