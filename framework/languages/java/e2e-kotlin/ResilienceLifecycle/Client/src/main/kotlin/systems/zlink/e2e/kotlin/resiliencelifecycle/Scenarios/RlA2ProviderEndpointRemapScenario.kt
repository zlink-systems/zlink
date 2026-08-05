package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runProviderEndpointRemapScenario() {
    waitForTopology(2)
    post("${adminB()}/admin/drain")
    waitForWeight(adminB(), 0)
    collectStableProvidersWithout("a2-before-reschedule", "api-b", "api-a")
    signal("a2-ready")
    waitForSignal("a2-down")
    expectSingleProviderDownFailure("RL-A2", "a2-down-window")
    signal("a2-down-observed")
    waitForSignal("a2-up")
    waitForTopologyEndpoint(
        "api-a",
        options.apiAReplacementEndpoint
            ?: throw IllegalStateException("e2e.api.a.replacement.endpoint is required"),
    )
    collectStableProvidersWithout("a2-after-reschedule", "api-b", "api-a")
    post("${adminB()}/admin/restore")
    waitForWeight(adminB(), 100)
    println("scenario RL-A2 passed")
}
