package Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios
import systems.zlink.e2e.kotlin.registrymessaging.client.Support
import java.util.UUID
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq

object RmC3MultiProviderDistributionScenario {
    fun run(directConsumer: HttpJson, providerA: HttpJson, providerB: HttpJson) {
        val beforeA = providerA.get<List<String>>("/evidence")
        val beforeB = providerB.get<List<String>>("/evidence")
        val marker = "rm-c3-${UUID.randomUUID().toString().replace("-", "")}"
        val requests = (0 until 60).map { ProfileReq("$marker-$it") }
        val replies = directConsumer.post<List<ProfileRes>>("/profile/batch-request", requests)
        ScenarioAssert.that(replies.size == requests.size, "RM-C3 reply count mismatch.")
        replies.forEachIndexed { index, reply ->
            ScenarioAssert.that(reply.value == "profile:$marker-$index", "RM-C3 reply value mismatch.")
            ScenarioAssert.that(reply.providerRid == "api-a" || reply.providerRid == "api-b", "RM-C3 reply provider mismatch.")
        }
        val afterA = providerA.get<List<String>>("/evidence")
        val afterB = providerB.get<List<String>>("/evidence")
        val a = ScenarioAssert.countNewEvidence(afterA, beforeA, "profile-request|rid=api-a", marker)
        val b = ScenarioAssert.countNewEvidence(afterB, beforeB, "profile-request|rid=api-b", marker)
        ScenarioAssert.that(a > 0 && b > 0 && a + b == requests.size, "RM-C3 expected both providers.")
        println("scenario RM-C3 passed")
    }
}
