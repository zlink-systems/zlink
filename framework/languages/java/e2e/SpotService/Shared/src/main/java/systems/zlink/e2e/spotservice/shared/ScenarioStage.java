package systems.zlink.e2e.spotservice.shared;

import java.time.Duration;
import java.util.concurrent.TimeUnit;

public final class ScenarioStage {
    private final UserSpot spot;

    public ScenarioStage(UserSpot spot) {
        this.spot = spot;
    }

    public Contracts.StateRes apply(Contracts.StageProbeReq request) {
        String value = spot.apply(request.marker() + "-" + request.delta());
        spot.record("StageRequest", request.marker() + "|" + request.delta());
        return new Contracts.StateRes(spot.spotRid(), spot.nodeRid(), value);
    }

    public Contracts.StageTimerStartRes startTimer(Contracts.StageTimerStartReq request) {
        try {
            spot.context().addTimer(
                    request.name(),
                    Duration.ofMillis(request.periodMilliseconds()),
                    StageTimerHandler.class,
                    null)
                .toCompletableFuture()
                .get(5, TimeUnit.SECONDS);
            return new Contracts.StageTimerStartRes(spot.spotRid(), request.name(), true);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("stage timer start interrupted", error);
        } catch (java.util.concurrent.ExecutionException | java.util.concurrent.TimeoutException error) {
            throw new IllegalStateException("stage timer start failed", error);
        }
    }
}
