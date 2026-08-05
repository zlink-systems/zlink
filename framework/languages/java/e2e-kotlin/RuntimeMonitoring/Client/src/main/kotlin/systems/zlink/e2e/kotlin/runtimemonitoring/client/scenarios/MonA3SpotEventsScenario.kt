package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient

class MonA3SpotEventsScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        evidence.waitForEvent(
            options.serviceHttp,
            "spot",
            setOf(
                "STATUS_CHANGED",
                "PEERS_CHANGED",
                "SUBJECTS_CHANGED",
                "TIMER_HANDLER_FAILED",
                "TIMER_STOPPED_AFTER_UNHANDLED_EXCEPTION",
            ),
        )
        println("scenario MON-A3 passed")
    }
}
