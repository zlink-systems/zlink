package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient

class MonA2RegistryEventsScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        evidence.waitForEvent(
            options.serviceHttp,
            "location",
            setOf("STATUS_CHANGED", "TOPOLOGY_CHANGED", "SERVICE_SUMMARY_CHANGED"),
        )
        println("scenario MON-A2 passed")
    }
}
