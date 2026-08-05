package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.DynamicClusterLauncher
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert

object RmA4SameRidFailoverScenario {
    fun run(options: ClientOptions) {
        DynamicClusterLauncher.start(options, "rm-a4").use { cluster ->
            val providerV1 = cluster.startProvider("api-a-v1", "api-a", instanceId = "api-a-v1")
            val consumer = cluster.startConsumer("consumer")
            val requester = HttpJson(consumer.httpUrl)
            cluster.waitPeerEndpoint(requester, providerV1.routingId)
            val first = ScenarioAssert.requestProfileEventually(requester, "failover-before")
            ScenarioAssert.that(
                first.providerRid == "api-a" && first.instanceId == "api-a-v1",
                "RM-A4 initial provider mismatch.",
            )

            cluster.stop(providerV1)
            cluster.waitPeerEndpointAbsent(requester, providerV1.routingId)
            val providerV2 = cluster.startProvider("api-a-v2", "api-a", instanceId = "api-a-v2")
            cluster.waitPeerEndpoint(requester, providerV2.routingId)
            repeat(20) { index ->
                val reply = ScenarioAssert.requestProfileEventually(requester, "failover-after-$index")
                ScenarioAssert.that(
                    reply.providerRid == "api-a" && reply.instanceId == "api-a-v2",
                    "RM-A4 did not switch to replacement provider.",
                )
            }
        }
        println("scenario RM-A4 passed")
    }
}
