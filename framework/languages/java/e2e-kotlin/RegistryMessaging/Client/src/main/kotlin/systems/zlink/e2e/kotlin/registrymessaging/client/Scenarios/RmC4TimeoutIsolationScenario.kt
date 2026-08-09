package Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios
import systems.zlink.e2e.kotlin.registrymessaging.client.Support
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.RequestFailureRes

object RmC4TimeoutIsolationScenario {
    fun run(discoveryConsumer: HttpJson, providerA: HttpJson, providerB: HttpJson) {
        val timeout = discoveryConsumer.post<RequestFailureRes>("/profile/slow-request", ProfileReq("slow"))
        ScenarioAssert.that(timeout.failed, "RM-C4 expected the slow request to time out.")

        val immediate = discoveryConsumer.post<ProfileRes>("/profile/request", ProfileReq("rm-c4-after-timeout"))
        ScenarioAssert.that(immediate.value == "profile:rm-c4-after-timeout", "RM-C4 follow-up reply mismatch.")
        Thread.sleep(1200)
        val later = discoveryConsumer.post<ProfileRes>("/profile/request", ProfileReq("rm-c4-later"))
        ScenarioAssert.that(later.value == "profile:rm-c4-later", "RM-C4 later reply mismatch.")

        val evidence = providerA.get<List<String>>("/evidence") + providerB.get<List<String>>("/evidence")
        ScenarioAssert.that(
            evidence.any { it.contains("rm-c4-after-timeout") } && evidence.any { it.contains("rm-c4-later") },
            "RM-C4 follow-up evidence missing.",
        )
        println("scenario RM-C4 passed")
    }
}
