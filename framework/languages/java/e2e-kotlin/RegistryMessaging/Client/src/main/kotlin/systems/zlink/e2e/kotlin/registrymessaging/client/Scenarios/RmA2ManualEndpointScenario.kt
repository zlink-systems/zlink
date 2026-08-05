package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq

object RmA2ManualEndpointScenario {
    fun run(singleConsumer: HttpJson, providerA: HttpJson) {
        val reply = singleConsumer.post<ProfileRes>("/profile/request", ProfileReq("rm-a2"))
        ScenarioAssert.that(reply.value == "profile:rm-a2", "RM-A2 reply value mismatch.")
        ScenarioAssert.that(reply.providerRid == "api-a", "RM-A2 manual endpoint should reach api-a.")
        val evidence = providerA.post<List<String>>("/evidence/wait", EvidenceWaitReq("value=rm-a2"))
        ScenarioAssert.that(evidence.any { it.contains("value=rm-a2") }, "RM-A2 api-a evidence missing.")
        println("scenario RM-A2 passed")
    }
}
