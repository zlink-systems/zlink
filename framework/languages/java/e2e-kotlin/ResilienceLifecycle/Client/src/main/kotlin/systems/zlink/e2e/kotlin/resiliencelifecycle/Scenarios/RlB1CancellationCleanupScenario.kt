package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.time.Duration

fun ClientScenarioContext.runClientTimeoutCleanupScenario() {
    try {
        requestWork("timeout", Duration.ofMillis(300))
        throw IllegalStateException("RL-B1 timeout request unexpectedly completed")
    } catch (_: RuntimeException) {
        waitForEvidenceAny("TimeoutStarted", adminA(), adminB())
    }
    sleep(1800)
    val followUp = requestWork("b1-follow-up")
    ensure(followUp.value() == "work:b1-follow-up", "RL-B1 follow-up payload mismatch")
    println("scenario RL-B1 passed")
}
