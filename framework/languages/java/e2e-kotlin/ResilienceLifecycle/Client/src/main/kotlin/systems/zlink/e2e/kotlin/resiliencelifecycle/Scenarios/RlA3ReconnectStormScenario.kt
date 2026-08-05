package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runReconnectStormScenario() {
    waitForTopology(2)
    val providers = collectProviders("a3-storm", 40, 1)
    ensure(providers.isNotEmpty(), "RL-A3 storm client did not receive replies")
    println("scenario RL-A3 passed")
    runHighFanoutEvidenceScenario()
    sleep(options.stormExitDelayMillis)
}
