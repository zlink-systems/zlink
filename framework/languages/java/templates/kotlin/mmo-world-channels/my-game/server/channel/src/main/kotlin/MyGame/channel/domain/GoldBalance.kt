package MyGame.channel.domain

import MyGame.ActionRejected
import MyGame.GameRules
import MyGame.Rejection
import java.util.concurrent.TimeUnit

class GoldBalance(
    initialGold: Long,
    cooldownRemainingMs: Long = 0,
    private val monotonicNanos: () -> Long = { System.nanoTime() },
) {
    data class Snapshot(val gold: Long, val cooldownRemainingMs: Long)

    data class Decision(
        val accepted: Boolean,
        val gold: Long,
        val earnedAtNanos: Long,
        val rejection: ActionRejected?,
    )

    private val earnIntervalNanos = TimeUnit.MILLISECONDS.toNanos(GameRules.EARN_INTERVAL_MS)
    var gold: Long = 0
        private set
    private var lastEarnAtNanos = 0L
    private var hasEarned = false

    init {
        restore(Snapshot(initialGold, cooldownRemainingMs))
    }

    fun previewEarn(): Decision {
        val now = monotonicNanos()
        if (hasEarned) {
            val remainingNanos = earnIntervalNanos - (now - lastEarnAtNanos)
            if (remainingNanos > 0) {
                val retryAfterMs = (remainingNanos + 999_999L) / 1_000_000L
                return Decision(
                    accepted = false,
                    gold = 0,
                    earnedAtNanos = 0,
                    rejection = ActionRejected(Rejection.EARN_TOO_SOON, Math.toIntExact(retryAfterMs)),
                )
            }
        }
        if (gold > GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT) {
            return Decision(
                accepted = false,
                gold = 0,
                earnedAtNanos = 0,
                rejection = ActionRejected(Rejection.GOLD_LIMIT, 0),
            )
        }
        return Decision(
            accepted = true,
            gold = gold + GameRules.EARN_AMOUNT,
            earnedAtNanos = now,
            rejection = null,
        )
    }

    fun commit(decision: Decision) {
        check(decision.accepted) { "Cannot commit a rejected earn" }
        gold = decision.gold
        lastEarnAtNanos = decision.earnedAtNanos
        hasEarned = true
    }

    fun capture(): Snapshot {
        val remainingNanos =
            if (hasEarned) earnIntervalNanos - (monotonicNanos() - lastEarnAtNanos) else 0
        val remainingMs =
            if (remainingNanos > 0) (remainingNanos + 999_999L) / 1_000_000L else 0
        return Snapshot(gold, remainingMs)
    }

    fun restore(snapshot: Snapshot) {
        require(
            snapshot.gold in 0..GameRules.MAX_INTEGER &&
                snapshot.cooldownRemainingMs in 0..GameRules.EARN_INTERVAL_MS,
        ) { "Invalid gold snapshot" }
        gold = snapshot.gold
        hasEarned = snapshot.cooldownRemainingMs > 0
        if (hasEarned) {
            val elapsedNanos =
                TimeUnit.MILLISECONDS.toNanos(GameRules.EARN_INTERVAL_MS - snapshot.cooldownRemainingMs)
            lastEarnAtNanos = monotonicNanos() - elapsedNanos
        }
    }
}
