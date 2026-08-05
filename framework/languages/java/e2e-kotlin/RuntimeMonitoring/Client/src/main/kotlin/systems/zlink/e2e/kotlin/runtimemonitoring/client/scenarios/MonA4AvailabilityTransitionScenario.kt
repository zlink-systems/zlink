package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonA4AvailabilityTransitionScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun runDrainSubset() {
        val before = evidence.post("${options.triggerHttp}/profile/request")
        ScenarioAssert.ensure(
            before.contains("\"providerRid\":\"svc-a\""),
            "MON-A4 trigger request did not hit svc-a before drain: $before",
        )

        evidence.post("${options.serviceHttp}/admin/drain")
        try {
            evidence.waitForEvent(
                options.triggerHttp,
                "socket",
                Contracts.CHANNEL,
                setOf("PEER_ADMISSION_CHANGED"),
            )
            evidence.waitForEvent(
                options.serviceHttp,
                "admin",
                Contracts.CHANNEL,
                setOf("WeightChanged"),
            )
            evidence.waitForEvent(
                options.serviceHttp,
                "location",
                Contracts.LOCATION_SOURCE,
                setOf("TOPOLOGY_CHANGED"),
            )
        } finally {
            evidence.post("${options.serviceHttp}/admin/restore")
        }

        println("scenario MON-A4 passed")
    }
}
