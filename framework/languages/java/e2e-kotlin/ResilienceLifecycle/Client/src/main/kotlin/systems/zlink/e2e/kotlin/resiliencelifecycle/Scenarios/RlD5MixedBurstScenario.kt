package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runMixedBurstScenarioBody() {
    var firstWindowNanos = 0L
    var lastWindowNanos = 0L
    val providers = linkedSetOf<String>()
    repeat(4) { window ->
        val started = System.nanoTime()
        repeat(40) { index ->
            val value = "d5-soak-$window-$index"
            if (index % 5 == 0) {
                sendWork(value)
            } else {
                val reply = requestWork(value)
                ensure(reply.value() == "work:$value", "RL-D5 reply payload mismatch for $value")
                providers.add(reply.providerRid())
            }
        }
        val elapsed = System.nanoTime() - started
        if (window == 0) {
            firstWindowNanos = elapsed
        }
        lastWindowNanos = elapsed
    }
    ensure(providers.isNotEmpty(), "RL-D5 did not observe request replies")
    ensure(lastWindowNanos < firstWindowNanos * 5, "RL-D5 latency drift exceeded the harness threshold")
}
