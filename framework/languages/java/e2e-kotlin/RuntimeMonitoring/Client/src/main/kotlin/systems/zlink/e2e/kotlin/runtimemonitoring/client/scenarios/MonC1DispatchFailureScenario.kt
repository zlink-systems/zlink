package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonC1DispatchFailureScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        repeat(12) { index ->
            val reply = evidence.postJson(
                "${options.triggerHttp}/profile/request/throw?value=c1-trigger-$index",
                Contracts.WorkRes::class.java,
            )
            ScenarioAssert.ensure(
                reply.value == "work:c1-trigger-$index",
                "MON-C1 trigger reply mismatch",
            )
            if (evidence.events(options.throwingServiceHttp, "monitoring")
                    .contains("HandlerFailureInjected")
            ) {
                evidence.waitForEvent(
                    options.throwingServiceHttp,
                    "socket",
                    Contracts.CHANNEL,
                    setOf("CONNECTION_READY"),
                )
                val recovery = evidence.postJson(
                    "${options.triggerHttp}/profile/request/throw?value=c1-after-handler-failure",
                    Contracts.WorkRes::class.java,
                )
                ScenarioAssert.ensure(
                    recovery.value == "work:c1-after-handler-failure",
                    "MON-C1 follow-up reply mismatch",
                )
                println("scenario MON-C1 passed")
                return
            }
        }

        val reply = evidence.postJson(
            "${options.triggerHttp}/profile/request/throw?value=c1-after-handler-failure",
            Contracts.WorkRes::class.java,
        )
        ScenarioAssert.ensure(
            reply.value == "work:c1-after-handler-failure",
            "MON-C1 follow-up reply mismatch",
        )
        evidence.waitForEvent(options.throwingServiceHttp, "monitoring", setOf("HandlerFailureInjected"))
        println("scenario MON-C1 passed")
    }
}
