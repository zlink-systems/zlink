package MyGame.channel;

import MyGame.*;

import java.util.concurrent.TimeUnit;
import java.util.function.LongSupplier;

public final class GoldBalance {
    public record Snapshot(long gold, long cooldownRemainingMs) {}

    public record Decision(boolean accepted, long gold, long earnedAtNanos, ActionRejected rejection) {
        static Decision accepted(long gold, long earnedAtNanos) {
            return new Decision(true, gold, earnedAtNanos, null);
        }

        static Decision rejected(String reason, long retryAfterMs) {
            return new Decision(false, 0, 0, new ActionRejected(reason, Math.toIntExact(retryAfterMs)));
        }
    }

    private static final long EARN_INTERVAL_NANOS =
            TimeUnit.MILLISECONDS.toNanos(GameRules.EARN_INTERVAL_MS);

    private final LongSupplier monotonicNanos;
    private long gold;
    private long lastEarnAtNanos;
    private boolean hasEarned;

    public GoldBalance(long gold) {
        this(gold, 0, System::nanoTime);
    }

    public GoldBalance(long gold, long cooldownRemainingMs, LongSupplier monotonicNanos) {
        this.monotonicNanos = monotonicNanos;
        restore(new Snapshot(gold, cooldownRemainingMs));
    }

    public long gold() {
        return gold;
    }

    public Decision previewEarn() {
        long now = monotonicNanos.getAsLong();
        if (hasEarned) {
            long remainingNanos = EARN_INTERVAL_NANOS - (now - lastEarnAtNanos);
            if (remainingNanos > 0)
                return Decision.rejected(
                        Rejection.EARN_TOO_SOON,
                        (remainingNanos + 999_999L) / 1_000_000L);
        }
        if (gold > GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT)
            return Decision.rejected(Rejection.GOLD_LIMIT, 0);
        return Decision.accepted(gold + GameRules.EARN_AMOUNT, now);
    }

    public void commit(Decision decision) {
        if (!decision.accepted()) throw new IllegalArgumentException("Cannot commit a rejected earn");
        gold = decision.gold();
        lastEarnAtNanos = decision.earnedAtNanos();
        hasEarned = true;
    }

    public Snapshot capture() {
        long remainingMs = 0;
        if (hasEarned) {
            long remainingNanos = EARN_INTERVAL_NANOS - (monotonicNanos.getAsLong() - lastEarnAtNanos);
            if (remainingNanos > 0) remainingMs = (remainingNanos + 999_999L) / 1_000_000L;
        }
        return new Snapshot(gold, remainingMs);
    }

    public void restore(Snapshot snapshot) {
        if (snapshot.gold() < 0
                || snapshot.gold() > GameRules.MAX_INTEGER
                || snapshot.cooldownRemainingMs() < 0
                || snapshot.cooldownRemainingMs() > GameRules.EARN_INTERVAL_MS)
            throw new IllegalArgumentException("Invalid gold snapshot");
        gold = snapshot.gold();
        hasEarned = snapshot.cooldownRemainingMs() > 0;
        if (hasEarned) {
            long elapsedNanos =
                    TimeUnit.MILLISECONDS.toNanos(
                            GameRules.EARN_INTERVAL_MS - snapshot.cooldownRemainingMs());
            lastEarnAtNanos = monotonicNanos.getAsLong() - elapsedNanos;
        }
    }
}
