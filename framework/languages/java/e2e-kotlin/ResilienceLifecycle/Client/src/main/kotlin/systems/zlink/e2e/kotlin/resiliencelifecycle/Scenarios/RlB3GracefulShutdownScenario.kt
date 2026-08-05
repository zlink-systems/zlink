package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runGracefulShutdownScenario() {
    waitForTopology(2)
    val beforeShutdown = requestWork("b3-before-shutdown")
    ensure(beforeShutdown.value() == "work:b3-before-shutdown", "RL-B3 pre-shutdown reply payload mismatch")

    post("${adminB()}/admin/shutdown")
    waitForTopology(1)
    val providers = collectStableProvidersWithout("b3-after-shutdown", "api-b", "api-a")
    ensure(providers.contains("api-a"), "RL-B3 did not converge to api-a after api-b shutdown")
    println("scenario RL-B3 passed")
}
