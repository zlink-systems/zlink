package MyGame.e2e

import MyGame.GameRules
import MyGame.Rejection
import MyGame.channel.domain.GoldBalance
import java.util.concurrent.atomic.AtomicLong

fun runDomainChecks(): Int {
    var checks = 0
    val now = AtomicLong()
    val balance = GoldBalance(0, 0, now::get)

    val first = balance.previewEarn()
    check(first.accepted) { "Initial earn was rejected" }
    checks++
    balance.commit(first)
    check(balance.gold == GameRules.EARN_AMOUNT) { "Wrong fixed reward" }
    checks++

    now.set(600_000_000L)
    val early = balance.previewEarn()
    val earlyRejection = early.rejection
    check(
        !early.accepted &&
            earlyRejection?.reason == Rejection.EARN_TOO_SOON &&
            earlyRejection.retryAfterMs == 400 &&
            balance.gold == GameRules.EARN_AMOUNT,
    ) { "Early earn changed state or returned a wrong delay" }
    checks++

    now.set(999_000_000L)
    val lastMs = balance.previewEarn()
    check(!lastMs.accepted && lastMs.rejection?.retryAfterMs == 1) { "Wrong last-ms delay" }
    checks++

    now.set(1_000_000_000L)
    val second = balance.previewEarn()
    check(second.accepted) { "Earn was rejected at the interval boundary" }
    balance.commit(second)
    check(balance.gold == 2 * GameRules.EARN_AMOUNT) { "Wrong balance after second earn" }
    checks++

    now.set(1_500_000_000L)
    val snapshot = balance.capture()
    check(snapshot.cooldownRemainingMs == 500L) { "Snapshot did not preserve remaining time" }
    checks++

    now.set(10_000_000_000L)
    val relocated = GoldBalance(snapshot.gold, snapshot.cooldownRemainingMs, now::get)
    val restored = relocated.previewEarn()
    check(!restored.accepted && restored.rejection?.retryAfterMs == 500) {
        "Relocation did not preserve remaining cooldown"
    }
    checks++

    now.set(10_500_000_000L)
    val afterCooldown = relocated.previewEarn()
    check(afterCooldown.accepted) { "Relocated earn was rejected at its boundary" }
    relocated.commit(afterCooldown)
    checks++

    val atLimit = GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT, 0, now::get)
    val boundary = atLimit.previewEarn()
    check(boundary.accepted) { "Gold limit boundary was rejected" }
    atLimit.commit(boundary)
    check(atLimit.gold == GameRules.MAX_INTEGER) { "Gold limit boundary was not reached" }
    checks++

    val overLimit =
        GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1, 0, now::get)
    val rejected = overLimit.previewEarn()
    val rejection = rejected.rejection
    check(
        !rejected.accepted &&
            rejection?.reason == Rejection.GOLD_LIMIT &&
            rejection.retryAfterMs == 0 &&
            overLimit.gold == GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1,
    ) { "Gold limit rejection changed state or returned a wrong reason" }
    checks++
    return checks
}
