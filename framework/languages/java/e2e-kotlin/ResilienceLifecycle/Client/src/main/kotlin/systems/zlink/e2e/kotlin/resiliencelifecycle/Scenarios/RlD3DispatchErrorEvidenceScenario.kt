package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runDispatchErrorEvidenceScenario() {
    try {
        requestUnhandled("d3-missing-handler")
        throw IllegalStateException("RL-D3 missing handler request unexpectedly completed")
    } catch (_: RuntimeException) {
        waitForDispatchErrorAny("UnhandledReq", adminA(), adminB())
    }
    val followUp = requestWork("d3-follow-up")
    ensure(followUp.value() == "work:d3-follow-up", "RL-D3 follow-up payload mismatch")
    println("scenario RL-D3 passed")
}
