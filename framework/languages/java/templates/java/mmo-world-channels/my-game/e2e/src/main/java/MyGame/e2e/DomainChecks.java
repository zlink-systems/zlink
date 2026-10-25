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
        require(first.accepted() && first.gold() == GameRules.EARN_AMOUNT, "Initial earn was rejected");
        checks++;
        require(balance.gold() == 0, "Gold changed before persistence completed");
        checks++;
        balance.commit(first);
        require(balance.gold() == GameRules.EARN_AMOUNT, "Wrong fixed reward");
        checks++;

        var early = balance.previewEarn();
        require(
                !early.accepted()
                        && Rejection.EARN_TOO_SOON.equals(early.rejection().reason())
                        && early.rejection().retryAfterMs() == GameRules.EARN_INTERVAL_MS,
                "Immediate earn did not return the full cooldown");
        checks++;
        require(
                balance.gold() == GameRules.EARN_AMOUNT
                        && balance.capture().cooldownRemainingMs() == GameRules.EARN_INTERVAL_MS,
                "Rejected earn changed gold or cooldown state");
        checks++;

        now.set(250_000_000L);
        var snapshot = balance.capture();
        require(snapshot.cooldownRemainingMs() == 750, "Snapshot did not store remaining milliseconds");
        checks++;

        now.set(10_000_000_000L);
        var relocated = new GoldBalance(snapshot.gold(), snapshot.cooldownRemainingMs(), now::get);
        var restored = relocated.previewEarn();
        require(
                !restored.accepted()
                        && restored.rejection().retryAfterMs() == 750,
                "Relocation did not preserve remaining cooldown");
        checks++;

        now.set(10_750_000_000L);
        var afterCooldown = relocated.previewEarn();
        require(
                afterCooldown.accepted() && afterCooldown.gold() == 2 * GameRules.EARN_AMOUNT,
                "Earn was not accepted after the restored cooldown");
        checks++;

        var atLimit = new GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT, 0, now::get);
        var boundary = atLimit.previewEarn();
        require(
                boundary.accepted() && boundary.gold() == GameRules.MAX_INTEGER,
                "Gold at maximum minus reward was not accepted exactly at the limit");
        checks++;
        atLimit.commit(boundary);
        now.addAndGet(1_000_000_000L);
        var atCap = atLimit.previewEarn();
        require(
                !atCap.accepted()
                        && Rejection.GOLD_LIMIT.equals(atCap.rejection().reason())
                        && atCap.rejection().retryAfterMs() == 0,
                "Gold limit was not rejected");
        checks++;
        require(atLimit.gold() == GameRules.MAX_INTEGER, "Gold limit rejection changed the balance");
        checks++;

        var overLimit =
                new GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1, 0, now::get);
        var rejected = overLimit.previewEarn();
        require(
                !rejected.accepted()
                        && Rejection.GOLD_LIMIT.equals(rejected.rejection().reason())
                        && rejected.rejection().retryAfterMs() == 0,
                "Gold at maximum minus reward plus one was not rejected");
        checks++;
        require(
                overLimit.gold() == GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1,
                "Rejected boundary earn changed the balance");
        checks++;
        return checks;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
