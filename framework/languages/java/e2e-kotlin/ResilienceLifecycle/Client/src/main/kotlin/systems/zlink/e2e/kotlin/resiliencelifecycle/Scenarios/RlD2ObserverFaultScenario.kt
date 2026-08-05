package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runObserverFaultScenario() {
    post("${adminA()}/admin/fault/observer-throws")
    post("${adminB()}/admin/fault/observer-throws")
    waitForEvidence(adminA(), "ObserverFaultMode")
    waitForEvidence(adminB(), "ObserverFaultMode")

    try {
        requestUnhandled("d2-observer-fault")
        throw IllegalStateException("RL-D2 missing handler request unexpectedly completed")
    } catch (_: RuntimeException) {
    } finally {
        post("${adminA()}/admin/fault/none")
        post("${adminB()}/admin/fault/none")
    }
    // Observer failures are written through structured logging and do not
    // create an application callback or evidence DTO.

    val followUp = requestWork("rl-d2-after")
    ensure(followUp.value() == "work:rl-d2-after", "RL-D2 follow-up payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-d2-after", adminA(), adminB())
    println("scenario RL-D2 passed")
}
