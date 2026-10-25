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
    check(first.accepted && first.gold == GameRules.EARN_AMOUNT) { "Initial earn was rejected" }
    checks++
    check(balance.gold == 0L) { "Gold changed before persistence completed" }
    checks++
    balance.commit(first)
    check(balance.gold == GameRules.EARN_AMOUNT) { "Wrong fixed reward" }
    checks++

    val early = balance.previewEarn()
    val earlyRejection = early.rejection
    check(
        !early.accepted &&
            earlyRejection?.reason == Rejection.EARN_TOO_SOON &&
            earlyRejection.retryAfterMs == GameRules.EARN_INTERVAL_MS.toInt(),
    ) { "Immediate earn did not return the full cooldown" }
    checks++
    check(
        balance.gold == GameRules.EARN_AMOUNT &&
            balance.capture().cooldownRemainingMs == GameRules.EARN_INTERVAL_MS,
    ) { "Rejected earn changed gold or cooldown state" }
    checks++

    now.set(250_000_000L)
    val snapshot = balance.capture()
    check(snapshot.cooldownRemainingMs == 750L) { "Snapshot did not store remaining milliseconds" }
    checks++

    now.set(10_000_000_000L)
    val relocated = GoldBalance(snapshot.gold, snapshot.cooldownRemainingMs, now::get)
    val restored = relocated.previewEarn()
    check(!restored.accepted && restored.rejection?.retryAfterMs == 750) {
        "Relocation did not preserve remaining cooldown"
    }
    checks++

    now.set(10_750_000_000L)
    val afterCooldown = relocated.previewEarn()
    check(
        afterCooldown.accepted && afterCooldown.gold == 2 * GameRules.EARN_AMOUNT,
    ) { "Earn was not accepted after the restored cooldown" }
    checks++

    val atLimit = GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT, 0, now::get)
    val boundary = atLimit.previewEarn()
    check(
        boundary.accepted && boundary.gold == GameRules.MAX_INTEGER,
    ) { "Gold at maximum minus reward was not accepted exactly at the limit" }
    checks++
    atLimit.commit(boundary)
    now.addAndGet(1_000_000_000L)
    val atCap = atLimit.previewEarn()
    val atCapRejection = atCap.rejection
    check(
        !atCap.accepted &&
            atCapRejection?.reason == Rejection.GOLD_LIMIT &&
            atCapRejection.retryAfterMs == 0,
    ) { "Gold limit was not rejected" }
    checks++
    check(atLimit.gold == GameRules.MAX_INTEGER) { "Gold limit rejection changed the balance" }
    checks++

    val overLimit =
        GoldBalance(GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1, 0, now::get)
    val rejected = overLimit.previewEarn()
    val rejection = rejected.rejection
    check(
        !rejected.accepted &&
            rejection?.reason == Rejection.GOLD_LIMIT &&
            rejection.retryAfterMs == 0,
    ) { "Gold at maximum minus reward plus one was not rejected" }
    checks++
    check(overLimit.gold == GameRules.MAX_INTEGER - GameRules.EARN_AMOUNT + 1) {
        "Rejected boundary earn changed the balance"
    }
    checks++
    return checks
}
