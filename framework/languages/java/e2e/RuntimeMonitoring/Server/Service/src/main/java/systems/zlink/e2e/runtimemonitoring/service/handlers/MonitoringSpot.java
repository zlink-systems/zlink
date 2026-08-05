package systems.zlink.e2e.runtimemonitoring.service.handlers;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class MonitoringSpot implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;

    public MonitoringSpot(ZLinkSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        ZLinkTimerOptions options = new ZLinkTimerOptions(
            systems.zlink.framework.spots.ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
            1,
            false);
        context.addTimer("failing-monitoring-timer", Duration.ofMillis(500),
            FailingTimerHandler.class, options);
        context.addTimer("stopping-monitoring-timer", Duration.ofMillis(500),
            FailingTimerHandler.class, null);
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public static final class FailingTimerHandler
        implements ZLinkSpotTimerHandler<MonitoringSpot> {
        @Override
        public CompletionStage<Void> handle(MonitoringSpot spot, ZLinkTimerTick tick) {
            if (tick.name().equals("failing-monitoring-timer") && tick.deliveryIndex() > 1) {
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(
                new IllegalStateException("monitoring timer boom"));
        }
    }

}
