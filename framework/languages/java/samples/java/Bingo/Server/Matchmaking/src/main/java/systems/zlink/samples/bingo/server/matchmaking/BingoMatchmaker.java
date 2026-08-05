package systems.zlink.samples.bingo.server.matchmaking;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;

public final class BingoMatchmaker implements ZLinkInstanceSpot {
    private static final Duration IDLE_CHECK_PERIOD = Duration.ofSeconds(5);
    private static final Duration IDLE_CLOSE_AFTER = Duration.ofSeconds(30);
    private final ZLinkInstanceSpotContext context;
    private final AtomicInteger inFlight = new AtomicInteger();
    private volatile Instant lastActivity = Instant.now();

    public BingoMatchmaker(ZLinkInstanceSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onInitialize() {
        return context.addTimer(
                "matchmaker-idle-close",
                IDLE_CHECK_PERIOD,
                BingoMatchmakerIdleTimerHandler.class,
                null)
            .thenApply(ignored -> null);
    }

    void beginRequest() {
        inFlight.incrementAndGet();
        lastActivity = Instant.now();
    }

    void endRequest() {
        lastActivity = Instant.now();
        inFlight.decrementAndGet();
    }

    void closeIfIdle() {
        if (inFlight.get() == 0
            && Duration.between(lastActivity, Instant.now()).compareTo(IDLE_CLOSE_AFTER) >= 0) {
            context.close();
        }
    }

}
