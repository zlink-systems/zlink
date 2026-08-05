package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runClientHostLifecycleAndMixedBurstScenario() {
    waitForTopology(2)
    runMixedBurstScenarioBody()
    println("scenario RL-C1 passed")
    println("scenario RL-D5 passed")
}
