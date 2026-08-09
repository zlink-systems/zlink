package Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios
import systems.zlink.e2e.kotlin.registrymessaging.client.Support
import java.util.concurrent.CompletableFuture
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.BackpressureRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg

object RmC9BackpressureScenario {
    private const val SLOW_SEND_COUNT = 8

    fun run(backpressureConsumer: HttpJson, providerA: HttpJson, providerB: HttpJson) {
        backpressureConsumer.post<Map<String, String>>("/profile/backpressure/reset")
        val ready = ScenarioAssert.requestProfileEventually(backpressureConsumer, "rm-c9-ready")
        ScenarioAssert.that(ready.value == "profile:rm-c9-ready", "RM-C9 initial connection did not become ready.")

        val futures = (0 until SLOW_SEND_COUNT).map { index ->
            CompletableFuture.supplyAsync {
                backpressureConsumer.post<BackpressureRes>(
                    "/profile/backpressure/send",
                    ProfileMsg("slow-c9-$index"),
                ).outcome
            }
        }
        val outcomes = futures.map { it.join() }
        ScenarioAssert.that(
            outcomes.all { it == "Submitted" },
            "RM-C9 expected all one-way sends to be submitted without a public bounded-failure oracle.",
        )

        Thread.sleep(10000)
        val recovered = ScenarioAssert.requestProfileEventually(backpressureConsumer, "c9-recovered")
        ScenarioAssert.that(recovered.value == "profile:c9-recovered", "RM-C9 connection did not recover after pressure.")
        val evidence = waitAnyEvidence(providerA, providerB, "c9-recovered")
        ScenarioAssert.that(evidence.any { it.contains("c9-recovered") }, "RM-C9 recovery evidence missing.")
        println("scenario RM-C9 passed")
    }

    private fun waitAnyEvidence(providerA: HttpJson, providerB: HttpJson, marker: String): List<String> =
        ScenarioAssert.retryUntil("provider evidence $marker") {
            val evidence = providerA.get<List<String>>("/evidence") + providerB.get<List<String>>("/evidence")
            if (evidence.any { it.contains(marker) }) {
                evidence
            } else {
                throw IllegalStateException("provider evidence missing")
            }
        }
}
