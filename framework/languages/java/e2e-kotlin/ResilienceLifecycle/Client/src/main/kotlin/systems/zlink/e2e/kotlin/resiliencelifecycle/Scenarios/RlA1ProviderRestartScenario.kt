package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runServerRestartScenario() {
    waitForTopology(2)
    post("${adminB()}/admin/drain")
    waitForWeight(adminB(), 0)
    collectStableProvidersWithout("a1-before-restart", "api-b", "api-a")
    signal("a1-ready")
    waitForSignal("a1-down")
    expectSingleProviderDownFailure("RL-A1", "a1-down-window")
    signal("a1-down-observed")
    waitForSignal("a1-up")
    waitForTopology(2)
    collectStableProvidersWithout("a1-after-restart", "api-b", "api-a")
    post("${adminB()}/admin/restore")
    waitForWeight(adminB(), 100)
    println("scenario RL-A1 passed")
    println("scenario RL-C3 passed")
}
