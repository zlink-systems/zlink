package systems.zlink.e2e.kotlin.resiliencelifecycle

fun ClientScenarioContext.runRuntimeDrainScenario() {
    waitForTopology(2)
    val warm = collectProviders("b4-warm", 80, 2)
    ensure(warm.contains("api-a") && warm.contains("api-b"), "RL-B4 warmup did not reach both providers: $warm")

    post("${adminA()}/admin/drain")
    waitForWeight(adminA(), 0)
    collectStableProvidersWithout("b4-drained", "api-a", "api-b")
    ensure(get("${adminA()}/health").contains("ok"), "RL-B4 drained provider health failed")
    waitForTopology(2)

    post("${adminA()}/admin/restore")
    waitForWeight(adminA(), 100)
    val restored = collectProviders("b4-restored", 120, 2)
    ensure(restored.contains("api-a"), "RL-B4 restored provider did not receive traffic")
    println("scenario RL-B4 passed")
}
