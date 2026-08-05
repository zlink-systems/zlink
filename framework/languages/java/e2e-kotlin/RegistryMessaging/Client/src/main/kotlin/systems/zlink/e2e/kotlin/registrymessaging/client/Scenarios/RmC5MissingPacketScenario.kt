package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.RequestFailureRes

object RmC5MissingPacketScenario {
    fun run(discoveryConsumer: HttpJson, providerA: HttpJson, providerB: HttpJson) {
        val missing = discoveryConsumer.post<RequestFailureRes>("/profile/missing-request", ProfileReq("missing-request"))
        ScenarioAssert.that(missing.failed, "RM-C5 missing request should fail.")
        discoveryConsumer.post<Map<String, Any>>("/profile/missing-command", ProfileMsg("missing-send"))

        val evidence = providerA.post<List<String>>("/evidence/wait", EvidenceWaitReq("MissingProfileReq")) +
            providerB.post<List<String>>("/evidence/wait", EvidenceWaitReq("MissingProfileMsg"))
        ScenarioAssert.that(
            evidence.any { it.contains("dispatch-error") && it.contains("MissingProfileReq") },
            "RM-C5 missing request evidence missing.",
        )
        ScenarioAssert.that(
            evidence.any { it.contains("dispatch-error") && it.contains("MissingProfileMsg") },
            "RM-C5 missing send evidence missing.",
        )

        val reply = discoveryConsumer.post<ProfileRes>("/profile/request", ProfileReq("rm-c5-after"))
        ScenarioAssert.that(reply.value == "profile:rm-c5-after", "RM-C5 normal request after negative path failed.")
        println("scenario RM-C5 passed")
    }
}
