package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runProviderFlappingScenario() {
    waitForTopology(2)
    signal("a5-ready")
    val providers = linkedSetOf<String>()
    var successes = 0
    var failures = 0
    var index = 0
    while (!hasSignal("a5-stop")) {
        try {
            val reply = requestWork("a5-flap-$index")
            ensure(reply.value() == "work:a5-flap-$index", "RL-A5 reply payload mismatch")
            providers.add(reply.providerRid())
            successes++
        } catch (_: RuntimeException) {
            failures++
            ensure(failures <= 5, "RL-A5 observed repeated failures during provider flapping")
        }
        index++
        sleep(100)
    }
    waitForTopology(2)
    val followUp = requestWork("a5-follow-up")
    ensure(followUp.value() == "work:a5-follow-up", "RL-A5 follow-up payload mismatch")
    ensure(successes >= 10, "RL-A5 did not send enough traffic during flapping")
    ensure(providers.contains("api-b"), "RL-A5 did not converge to live api-b during flapping")
    println("scenario RL-A5 passed")
}
