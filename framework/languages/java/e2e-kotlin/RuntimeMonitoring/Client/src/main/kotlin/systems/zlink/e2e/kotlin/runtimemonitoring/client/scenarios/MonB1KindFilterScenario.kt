package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonB1KindFilterScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        val reply = evidence.postJson(
            "${options.triggerHttp}/profile/request/service-b?value=mon-b1-request",
            Contracts.WorkRes::class.java,
        )
        ScenarioAssert.ensure(reply.providerRid == "svc-b", "MON-B1 direct trigger did not hit filtered service")

        evidence.waitForEvent(
            options.filteredServiceHttp,
            "socket",
            Contracts.CHANNEL,
            setOf("CONNECTION_READY"),
        )
        val observed = evidence.events(options.filteredServiceHttp, "socket", Contracts.CHANNEL)
        ScenarioAssert.ensure(
            observed.contains("CONNECTION_READY"),
            "MON-B1 did not observe filtered CONNECTION_READY event",
        )
        ScenarioAssert.ensure(
            observed == setOf("CONNECTION_READY"),
            "MON-B1 socket filter allowed unexpected events: $observed",
        )
        println("scenario MON-B1 passed")
    }
}
