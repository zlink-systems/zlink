package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq

object RmC1RequestSendScenario {
    fun run(providerA: HttpJson, providerB: HttpJson) {
        val reply = providerA.post<ProfileRes>("/profile/request", ProfileReq("rm-c1-request"))
        ScenarioAssert.that(reply.value == "profile:rm-c1-request", "RM-C1 request reply mismatch.")

        val commandId = "cmd-${UUID.randomUUID().toString().replace("-", "")}"
        providerA.post<Map<String, Any>>("/profile/command", ProfileMsg(commandId))
        val evidence = providerA.post<List<String>>("/evidence/wait", EvidenceWaitReq(commandId)) +
            providerB.post<List<String>>("/evidence/wait", EvidenceWaitReq(commandId))
        ScenarioAssert.that(evidence.any { it.contains("profile-request|") && it.contains("rm-c1-request") }, "RM-C1 request evidence missing.")
        ScenarioAssert.that(evidence.any { it.contains("profile-command|") }, "RM-C1 send evidence missing.")
        println("scenario RM-C1 passed")
    }
}
