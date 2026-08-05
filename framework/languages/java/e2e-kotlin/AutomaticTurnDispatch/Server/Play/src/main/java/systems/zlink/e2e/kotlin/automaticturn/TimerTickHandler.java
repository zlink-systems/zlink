package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class TimerTickHandler implements ZLinkSpotTimerHandler<ProbeSpot> {
    private final PlayEvidenceStore evidence;

    public TimerTickHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        ZLinkTimerTick tick) {
        ProbeSpot.TimerScenario scenario = spot.timerScenario(tick.name());
        if (scenario == null) {
            return CompletableFuture.completedFuture(null);
        }
        String value = "spot=" + spot.context().spotRid()
            + ";timer=" + tick.name()
            + ";mailbox=timer:" + tick.name()
            + ";tick=" + tick.deliveryIndex();
        if (tick.deliveryIndex() == 1
            && ("await-on-first".equals(scenario.mode()) || "await-then-next".equals(scenario.mode()))) {
            evidence.record(scenario.requestId(), "timer-await-started", value);
            evidence.record(scenario.requestId(), "timer-await-released", value);
            return spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(scenario.requestId(), scenario.delayMillis()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
                .thenAccept(reply -> {
                    evidence.record(scenario.requestId(), "timer-await-resumed", value);
                    evidence.record(scenario.requestId(), "timer-await-completed", value);
                    if ("await-on-first".equals(scenario.mode())) {
                        spot.closeTimer(tick.name());
                    }
                });
        }
        if (tick.deliveryIndex() == 2 && "await-then-next".equals(scenario.mode())) {
            evidence.record(scenario.requestId(), "timer-next-started", value);
            evidence.record(scenario.requestId(), "timer-next-completed", value);
            spot.closeTimer(tick.name());
            return CompletableFuture.completedFuture(null);
        }
        if ("fast".equals(scenario.mode())) {
            evidence.record(scenario.requestId(), "timer-fast-started", value);
            evidence.record(scenario.requestId(), "timer-fast-completed", value);
            spot.closeTimer(tick.name());
        }
        return CompletableFuture.completedFuture(null);
    }
}
