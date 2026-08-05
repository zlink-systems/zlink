package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonA1SocketEventsScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        val reply = evidence.postJson(
            "${options.triggerHttp}/profile/request/disconnect",
            Contracts.WorkRes::class.java,
        )
        ScenarioAssert.ensure(reply.value == "work:mon-a1-request", "MON-A1 trigger request failed")
        evidence.waitForEvent(
            options.serviceHttp,
            "socket",
            Contracts.CHANNEL,
            setOf("CONNECTION_READY"),
        )
        println("scenario MON-A1 passed")
    }
}
