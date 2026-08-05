package systems.zlink.e2e.spotservice.shared;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;

public final class TimerScenarioSpot implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;
    private final ScenarioState evidence;
    private Instant lastActivity = Instant.now();
    private int tickCount;
    private long skippedTicks;
    private String status = "open";
    private ZLinkTimer overrunTimer;
    private boolean idleKeepRecorded;

    @Override
    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public TimerScenarioSpot(
        ZLinkSpotContext context,
        ScenarioState evidence) {
        this.context = context;
        this.evidence = evidence;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(TimerActivityReqHandler.class);
        context.handlers().addHandler(TimerActivityResHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        String rid = context.spotId();
        if (rid.startsWith("timer-overrun-")) {
            ZLinkTimerOverrunPolicy overrunPolicy = rid.endsWith("catchup")
                ? ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED
                : rid.endsWith("delay")
                    ? ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK
                    : ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS;
            ZLinkTimerOptions options = new ZLinkTimerOptions(
                overrunPolicy,
                rid.endsWith("catchup") ? 2 : 1,
                false);
            context.addTimer("overrun", Duration.ofMillis(50), TimerOverrunHandler.class, options)
                .thenAccept(timer -> overrunTimer = timer);
            evidence.record("TimerOverrunConfigured", rid, options.overrunPolicy().name());
        } else {
            context.addTimer("idle", Duration.ofMillis(250), IdleCloseTimerHandler.class, null);
            evidence.record("IdleTimerConfigured", rid, "idle");
        }
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onClosing() {
        status = "closed";
        evidence.record("IdleClosed", context.spotId(), "closed");
        return CompletableFuture.completedFuture(null);
    }

    public void activity(String value) {
        lastActivity = Instant.now();
        status = "active:" + value;
        evidence.record("IdleActivity", context.spotId(), value);
    }

    public Contracts.TimerActivityRes status() {
        return new Contracts.TimerActivityRes(context.spotId(), status);
    }

    public void idleTick() {
        String rid = context.spotId();
        if ("idle-close".equals(rid)
            && Duration.between(lastActivity, Instant.now()).compareTo(Duration.ofMillis(700)) > 0) {
            evidence.record("IdleCloseRequested", rid, "idle");
            context.close();
            return;
        }
        if ("idle-active".equals(rid)) {
            if (!idleKeepRecorded) {
                idleKeepRecorded = true;
                evidence.record("IdleKeptOpen", rid, status);
            }
        }
    }

    public void overrunTick(long deliveryIndex, long skipped) {
        tickCount++;
        skippedTicks += skipped;
        String rid = context.spotId();
        status = "ticks=" + tickCount + ",skipped=" + skippedTicks;
        evidence.record("TimerOverrunTick", rid, deliveryIndex + "/" + skipped + "/" + tickCount);
        if (tickCount >= 3 && overrunTimer != null) {
            overrunTimer.close();
            evidence.record("TimerOverrunStopped", rid, status);
        }
    }
}
