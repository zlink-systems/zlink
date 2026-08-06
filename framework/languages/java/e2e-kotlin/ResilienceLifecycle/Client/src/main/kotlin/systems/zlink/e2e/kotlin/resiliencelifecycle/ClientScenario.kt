package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.ObjectMapper

class ClientScenario(
    json: ObjectMapper,
    options: ClientOptions,
) {
    private val context = ClientScenarioContext(json, options)

    fun run(mode: String) {
        when (mode) {
            "restart" -> context.runServerRestartScenario()
            "reschedule" -> context.runProviderEndpointRemapScenario()
            "flapping" -> context.runProviderFlappingScenario()
            "storm" -> context.runReconnectStormScenario()
            "cleanup" -> context.runClientHostLifecycleAndMixedBurstScenario()
            else -> {
                runDefaultScenarios()
            }
        }
    }

    private fun runDefaultScenarios() {
        when (context.options.scenario) {
            "all" -> {
                context.runClientTimeoutCleanupScenario()
                context.runRuntimeDrainScenario()
                context.runDrainInflightScenario()
                context.runDispatchErrorEvidenceScenario()
                context.runObserverFaultScenario()
                context.runMissingRequestHandlerScenario()
                context.runGrayFaultScenario()
                context.runGracefulShutdownScenario()
            }
            "RL-B1" -> context.runClientTimeoutCleanupScenario()
            "RL-A4" -> context.runDrainAndGreenEndpointScenario()
            "RL-B2" -> context.runCrashDuringInflightScenario()
            "RL-B4" -> context.runRuntimeDrainScenario()
            "RL-B5" -> context.runDrainInflightScenario()
            "RL-D3" -> context.runDispatchErrorEvidenceScenario()
            "RL-D2" -> context.runObserverFaultScenario()
            "RL-D4" -> context.runMissingRequestHandlerScenario()
            "RL-B6" -> context.runGrayFaultScenario()
            "RL-B3" -> context.runGracefulShutdownScenario()
            "RL-C2" -> context.runTopologyRecoveryScenario()
            "RL-C4" -> context.runLocationStoreOutageScenario()
            "RL-E1", "RL-E2", "RL-E3", "RL-E4", "RL-E5",
            "RL-F1", "RL-F2", "RL-F3", "RL-F4", "RL-F5", "RL-F6", "RL-F7",
            "RL-F8", "RL-F9", "RL-F10", "RL-F11", "RL-F12", "RL-F13", "RL-F14" ->
                context.runCommonScenarioGap(context.options.scenario)
            else -> throw IllegalArgumentException("unknown ResilienceLifecycle scenario ${context.options.scenario}")
        }
    }
}
