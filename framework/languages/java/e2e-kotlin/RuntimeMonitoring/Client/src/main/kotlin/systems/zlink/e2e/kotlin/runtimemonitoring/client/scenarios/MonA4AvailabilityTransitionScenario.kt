package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ReplacementSupport
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

    fun runReplacement(crash: Boolean, scenario: String) {
        val replacement = ReplacementSupport(options)
        val control = if (crash) "crash" else "shutdown"
        val stopped = evidence.postRaw("${options.filteredServiceHttp}/$control")
        ScenarioAssert.ensure(stopped.first in 200..299, "$scenario service-b did not stop: $stopped")
        replacement.waitForPort(false, "$scenario old service-b remained reachable")
        replacement.waitForMesh(false, "$scenario old service-b mesh remained bound")
        val process = replacement.startFiltered()
        try {
            replacement.waitForPort(true, "$scenario replacement did not start")
            replacement.waitForMesh(true, "$scenario replacement mesh did not start")
            awaitReady(scenario)
            val reply = evidence.post("${options.triggerHttp}/profile/request/service-b?value=${scenario.lowercase()}")
            ScenarioAssert.ensure(reply.contains("\"providerRid\":\"svc-b\""), "$scenario request was not handled by replacement: $reply")
            println("scenario $scenario passed")
        } finally {
            evidence.postBestEffort("${options.filteredServiceHttp}/shutdown")
            replacement.stop(process)
        }
    }

    private fun awaitReady(scenario: String) {
        repeat(200) {
            val status = evidence.json("${options.triggerHttp}/runtime/channel-snapshot")
            if (status.path("ready").asBoolean() && status.path("readyTargetCount").asInt() > 0) return
            ScenarioAssert.sleep(100)
        }
        throw IllegalStateException("$scenario replacement did not become READY")
    }
}
