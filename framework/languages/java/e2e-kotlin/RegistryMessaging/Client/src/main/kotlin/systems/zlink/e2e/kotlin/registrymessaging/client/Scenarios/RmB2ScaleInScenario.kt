package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.DynamicClusterLauncher
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert

object RmB2ScaleInScenario {
    fun run(options: ClientOptions) {
        DynamicClusterLauncher.start(options, "rm-b2").use { cluster ->
            val providerA = cluster.startProvider("api-a-scale-in", "api-a")
            val providerB = cluster.startProvider("api-b-scale-in", "api-b")
            val consumer = cluster.startConsumer("consumer-scale-in")
            val requester = HttpJson(consumer.httpUrl)
            cluster.waitPeerEndpoint(requester, providerA.routingId)
            cluster.waitPeerEndpoint(requester, providerB.routingId)
            cluster.waitPeerCount(requester, 2)
            val before = ScenarioAssert.requestUntilProvidersSeen(
                requester,
                "scale-in-before",
                setOf("api-a", "api-b"),
            )
            ScenarioAssert.that(before.contains("api-a") && before.contains("api-b"), "RM-B2 did not start with both providers.")
            cluster.stop(providerB)
            cluster.waitPeerEndpointAbsent(requester, providerB.routingId)
            repeat(20) { afterIndex ->
                val reply = ScenarioAssert.requestProfileEventually(requester, "scale-in-after-$afterIndex")
                ScenarioAssert.that(reply.providerRid == "api-a", "RM-B2 routed to removed provider.")
            }
        }
        println("scenario RM-B2 passed")
    }
}
