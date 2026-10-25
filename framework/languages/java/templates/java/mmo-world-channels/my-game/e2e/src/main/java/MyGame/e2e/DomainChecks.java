package MyGame.e2e;

import MyGame.*;
import MyGame.channel.GoldBalance;

import java.util.concurrent.atomic.AtomicLong;

final class DomainChecks {
    private DomainChecks() {}

    static int run() {
        int checks = 0;
        var now = new AtomicLong();
        var balance = new GoldBalance(0, 0, now::get);

        var first = balance.previewEarn();
        require(first.accepted(), "Initial earn was rejected");
        checks++;
        balance.commit(first);
        require(balance.gold() == GameRules.EARN_AMOUNT, "Wrong fixed reward");
        checks++;

        now.set(600_000_000L);
        var early = balance.previewEarn();
        require(
                !early.accepted()
                        && Rejection.EARN_TOO_SOON.equals(early.rejection().reason())
                        && early.rejection().retryAfterMs() == 400
                        && balance.gold() == GameRules.EARN_AMOUNT,
                "Early earn changed state or returned a wrong delay");
        checks++;

        now.set(999_000_000L);
        var lastMs = balance.previewEarn();
        require(!lastMs.accepted() && lastMs.rejection().retryAfterMs() == 1, "Wrong last-ms delay");
        checks++;

        now.set(1_000_000_000L);
        var second = balance.previewEarn();
        require(second.accepted(), "Earn was rejected at the interval boundary");
        balance.commit(second);
        require(balance.gold() == 2 * GameRules.EARN_AMOUNT, "Wrong balance after second earn");
        checks++;

        now.set(1_500_000_000L);
        var snapshot = balance.capture();
        require(snapshot.cooldownRemainingMs() == 500, "Snapshot did not preserve remaining time");
        checks++;

        now.set(10_000_000_000L);
        var relocated = new GoldBalance(snapshot.gold(), snapshot.cooldownRemainingMs(), now::get);
        var restored = relocated.previewEarn();
        require(
                !restored.accepted()
                        && restored.rejection().retryAfterMs() == 500,
                "Relocation did not preserve remaining cooldown");
        checks++;

        now.set(10_500_000_000L);
        var afterCooldown = relocated.previewEarn();
        require(afterCooldown.accepted(), "Relocated earn was rejected at its boundary");
        relocated.commit(afterCooldown);
        checks++;

        var atLimit = new GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT, 0, now::get);
        var boundary = atLimit.previewEarn();
        require(boundary.accepted(), "Gold limit boundary was rejected");
        atLimit.commit(boundary);
        require(atLimit.gold() == GameRules.MAX_INTEGER, "Gold limit boundary was not reached");
        checks++;

        var overLimit =
                new GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1, 0, now::get);
        var rejected = overLimit.previewEarn();
        require(
                !rejected.accepted()
                        && Rejection.GOLD_LIMIT.equals(rejected.rejection().reason())
                        && rejected.rejection().retryAfterMs() == 0
                        && overLimit.gold() == GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1,
                "Gold limit rejection changed state or returned a wrong reason");
        checks++;
        return checks;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
