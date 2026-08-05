package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.DynamicClusterLauncher
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert

object RmC7WeightedProviderScenario {
    fun run(options: ClientOptions) {
        DynamicClusterLauncher.start(options, "rm-c7").use { cluster ->
            val providerA = cluster.startProvider("api-a-weighted", "api-a", weight = 75)
            val providerB = cluster.startProvider("api-b-weighted", "api-b", weight = 25)
            val consumer = cluster.startConsumer("weighted-consumer")
            val requester = HttpJson(consumer.httpUrl)
            cluster.waitPeerEndpoint(requester, providerA.routingId)
            cluster.waitPeerEndpoint(requester, providerB.routingId)
            cluster.waitPeerCount(requester, 2)

            val providerAHttp = HttpJson(providerA.httpUrl)
            val providerBHttp = HttpJson(providerB.httpUrl)
            val beforeA = providerAHttp.get<List<String>>("/evidence")
            val beforeB = providerBHttp.get<List<String>>("/evidence")
            val marker = "rm-c7-${UUID.randomUUID().toString().replace("-", "")}"
            val replies = (0 until 240).map { index ->
                ScenarioAssert.requestProfileEventually(requester, "$marker-$index")
            }
            ScenarioAssert.that(replies.all { it.providerRid == "api-a" || it.providerRid == "api-b" }, "RM-C7 reply provider mismatch.")
            val afterA = providerAHttp.get<List<String>>("/evidence")
            val afterB = providerBHttp.get<List<String>>("/evidence")
            val a = ScenarioAssert.countNewEvidence(afterA, beforeA, "profile-request|rid=api-a", marker)
            val b = ScenarioAssert.countNewEvidence(afterB, beforeB, "profile-request|rid=api-b", marker)
            ScenarioAssert.that(a > 0 && b > 0 && a + b == 240 && a > b, "RM-C7 weighted provider counts did not favor api-a.")
        }
        println("scenario RM-C7 passed")
    }
}
