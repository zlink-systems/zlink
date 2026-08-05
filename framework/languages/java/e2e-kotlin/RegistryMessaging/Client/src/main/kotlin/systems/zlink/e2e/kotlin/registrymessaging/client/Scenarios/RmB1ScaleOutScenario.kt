package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.DynamicClusterLauncher
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq

object RmB1ScaleOutScenario {
    fun run(options: ClientOptions) {
        DynamicClusterLauncher.start(options, "rm-b1").use { cluster ->
            val providerA = cluster.startProvider("api-a-scale-out", "api-a")
            val consumer = cluster.startConsumer("consumer-scale-out")
            val requester = HttpJson(consumer.httpUrl)
            cluster.waitPeerEndpoint(requester, providerA.routingId)
            repeat(5) { index ->
                val reply = requester.post<ProfileRes>("/profile/request", ProfileReq("scale-out-before-$index"))
                ScenarioAssert.that(reply.providerRid == "api-a", "RM-B1 initial traffic should only use api-a.")
            }
            val providerB = cluster.startProvider("api-b-scale-out", "api-b")
            cluster.waitPeerEndpoint(requester, providerB.routingId)
            cluster.waitPeerCount(requester, 2)
            val providers = mutableSetOf<String>()
            var index = 0
            while (index < 100 && providers.size < 2) {
                providers += requester.post<ProfileRes>(
                    "/profile/request",
                    ProfileReq("scale-out-after-$index"),
                ).providerRid
                index++
            }
            ScenarioAssert.that(providers.contains("api-a") && providers.contains("api-b"), "RM-B1 did not route to both providers after scale-out.")
        }
        println("scenario RM-B1 passed")
    }
}
