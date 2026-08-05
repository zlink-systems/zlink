package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq

object RmA1DiscoveryRequestScenario {
    fun run(providerA: HttpJson, providerB: HttpJson, discoveryConsumer: HttpJson) {
        val reply = discoveryConsumer.post<ProfileRes>("/profile/request", ProfileReq("rm-a1"))
        ScenarioAssert.that(reply.value == "profile:rm-a1", "RM-A1 reply value mismatch.")
        ScenarioAssert.that(reply.providerRid == "api-a" || reply.providerRid == "api-b", "RM-A1 provider rid mismatch.")

        val peers = discoveryConsumer.get<List<Map<String, Any?>>>("/locations/peers")
        val ready = peers.count {
            it["meshName"] == Contracts.PROFILE_CHANNEL && it["role"] == "ROUTER"
        }
        ScenarioAssert.that(ready >= 2, "RM-A1 expected two live provider peer rows.")

        val evidence = providerA.get<List<String>>("/evidence") + providerB.get<List<String>>("/evidence")
        ScenarioAssert.that(evidence.any { it.contains("value=rm-a1") }, "RM-A1 provider evidence missing.")
        println("scenario RM-A1 passed")
    }
}
