package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runGrayFaultScenario() {
    post("${adminA()}/admin/fault-on")
    waitForEvidence(adminA(), "GrayFailureMode")
    var successes = 0
    var failures = 0
    val providers = linkedSetOf<String>()
    var index = 0
    while (index < 80 && successes < 10) {
        try {
            val reply = requestWork("b6-gray-$index")
            ensure(reply.value() == "work:b6-gray-$index", "RL-B6 reply payload mismatch")
            providers.add(reply.providerRid())
            successes++
        } catch (_: RuntimeException) {
            failures++
        }
        index++
    }
    post("${adminA()}/admin/fault-off")
    waitForEvidence(adminA(), "GrayFailureInjected")
    ensure(failures > 0, "RL-B6 did not observe public failures from degraded provider")
    ensure(successes >= 10, "RL-B6 healthy provider did not maintain enough successful traffic")
    ensure(providers.contains("api-b"), "RL-B6 did not receive successful replies from api-b")
    val followUp = requestWork("b6-follow-up")
    ensure(followUp.value() == "work:b6-follow-up", "RL-B6 follow-up payload mismatch")
    println("scenario RL-B6 passed")
}
