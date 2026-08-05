package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runDrainAndGreenEndpointScenario() {
    val greenApi = options.apiBGreenEndpoint
        ?: throw IllegalStateException("e2e.api.b.green.endpoint is required")
    val greenHttp = options.httpBGreenEndpoint
        ?: throw IllegalStateException("e2e.http.b.green.endpoint is required")

    waitForTopology(2)
    post("${adminB()}/admin/drain")
    waitForWeight(adminB(), 0)
    collectStableProvidersWithout("a4-rolling", "api-b", "api-a")

    signal("a4-drained")
    waitForSignal("a4-green-up")
    waitForTopologyEndpoint("api-b", greenApi)
    driveProviderTraffic("a4-green", greenHttp)

    signal("a4-green-served")
    waitForSignal("a4-green-down")
    waitForTopologyMissing("api-b")

    signal("a4-restore")
    waitForSignal("a4-restored")
    waitForTopologyEndpoint(
        "api-b",
        options.apiBEndpoint ?: throw IllegalStateException("e2e.api.b.endpoint is required"),
    )
    driveProviderTraffic("a4-restored", adminB())
    println("scenario RL-A4 passed")
}

private fun ClientScenarioContext.driveProviderTraffic(prefix: String, providerHttp: String) {
    repeat(120) { index ->
        val marker = "$prefix-$index"
        val reply = requestWork(marker)
        ensure(reply.value() == "work:$marker", "RL-A4 $prefix payload mismatch")
        if (reply.providerRid() == "api-b") {
            waitForEvidenceValue(providerHttp, "WorkReq", marker)
            return
        }
        sleep(100)
    }
    throw IllegalStateException("RL-A4 $prefix traffic did not reach api-b")
}
