package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runMissingRequestHandlerScenario() {
    val result = requestUnhandledRaw("d4-missing-handler")
    ensure(result.status >= 500, "RL-D4 missing handler request unexpectedly succeeded with ${result.status}")
    ensure(
        result.body.contains("HANDLER_MISSING") && result.body.contains("UnhandledReq"),
        "RL-D4 missing handler response did not expose a public request failure: ${result.body}",
    )
    waitForDispatchErrorAny("UnhandledReq", adminA(), adminB())

    val followUp = requestWork("d4-follow-up")
    ensure(followUp.value() == "work:d4-follow-up", "RL-D4 follow-up payload mismatch")
    println("scenario RL-D4 passed")
}
