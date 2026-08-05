package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.time.Duration
import java.util.concurrent.TimeUnit

fun ClientScenarioContext.runCrashDuringInflightScenario() {
    post("${adminA()}/admin/drain")
    waitForWeight(adminA(), 0)

    val slowValue = "b2-slow-inflight"
    val inFlight = submitWork(slowValue, Duration.ofSeconds(20))
    waitForEvidenceValueAny("SlowStarted", slowValue, adminB())
    signal("b2-ready")
    waitForSignal("b2-crashed")

    var failed = false
    try {
        inFlight.get(25, TimeUnit.SECONDS)
    } catch (_: Exception) {
        failed = true
    }
    ensure(failed, "RL-B2 in-flight request unexpectedly completed after provider crash")

    post("${adminA()}/admin/restore")
    waitForWeight(adminA(), 100)
    val followUp = requestWork("rl-b2-after-crash", Duration.ofSeconds(5))
    ensure(followUp.providerRid() == "api-a", "RL-B2 surviving provider traffic used ${followUp.providerRid()}")
    ensure(followUp.value() == "work:rl-b2-after-crash", "RL-B2 surviving provider payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-b2-after-crash", adminA())

    signal("b2-restart")
    waitForSignal("b2-restarted")
    waitUntilProviderServes("api-b", "rl-b2-restored")
    println("scenario RL-B2 passed")
}

private fun ClientScenarioContext.waitUntilProviderServes(providerRid: String, prefix: String) {
    repeat(40) { attempt ->
        try {
            val reply = requestWork("$prefix-$attempt", Duration.ofSeconds(5))
            if (reply.providerRid() == providerRid) {
                ensure(reply.value() == "work:$prefix-$attempt", "RL-B2 restored provider payload mismatch")
                return
            }
        } catch (_: RuntimeException) {
        }
        sleep(250)
    }
    throw IllegalStateException("RL-B2 restored provider $providerRid did not receive traffic")
}
