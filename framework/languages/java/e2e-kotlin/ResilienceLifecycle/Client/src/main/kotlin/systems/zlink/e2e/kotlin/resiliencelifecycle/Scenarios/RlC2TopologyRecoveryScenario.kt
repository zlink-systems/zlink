package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.time.Duration

fun ClientScenarioContext.runTopologyRecoveryScenario() {
    waitForTopology(2)
    val before = requestWork("rl-c2-before-crash", Duration.ofSeconds(5))
    ensure(before.value() == "work:rl-c2-before-crash", "RL-C2 before-crash payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-c2-before-crash", adminA(), adminB())

    signal("c2-ready")
    waitForSignal("c2-crashed")
    waitForTopologyMissing("api-b")

    repeat(8) { index ->
        val reply = requestWork("rl-c2-after-crash-$index", Duration.ofSeconds(5))
        ensure(reply.providerRid() == "api-a", "RL-C2 request used stale provider ${reply.providerRid()}")
        ensure(reply.value() == "work:rl-c2-after-crash-$index", "RL-C2 after-crash payload mismatch")
    }
    waitForEvidenceValueAny("WorkReq", "rl-c2-after-crash-0", adminA())

    signal("c2-restart")
    waitForSignal("c2-restarted")
    waitForTopology(2)
    waitUntilProviderServesAfterRecovery("api-b", "rl-c2-restored")
    println("scenario RL-C2 passed")
}

private fun ClientScenarioContext.waitUntilProviderServesAfterRecovery(providerRid: String, prefix: String) {
    repeat(40) { attempt ->
        val value = "$prefix-$attempt"
        try {
            val reply = requestWork(value, Duration.ofSeconds(5))
            ensure(reply.value() == "work:$value", "RL-C2 restored provider payload mismatch")
            if (reply.providerRid() == providerRid) {
                waitForEvidenceValueAny("WorkReq", value, adminB())
                return
            }
        } catch (_: RuntimeException) {
        }
        sleep(250)
    }
    throw IllegalStateException("RL-C2 restored provider $providerRid did not receive traffic")
}
