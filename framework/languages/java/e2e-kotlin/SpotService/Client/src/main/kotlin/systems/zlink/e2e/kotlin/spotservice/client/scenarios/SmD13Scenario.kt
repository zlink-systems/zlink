package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmD13Scenario {
    suspend fun run() {
        val actorId = "actor-sm-d13-heartbeat"
        val profile = Contracts.ActorProfile("Heartbeat", 13, listOf("heartbeat"))
        val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            connector.connect().await()
            val auth = connector
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D13 auth actor mismatch")

            Thread.sleep(700)
            ensure(connector.isConnected, "SM-D13 heartbeat-enabled stream disconnected")

            val reply = connector
                .request(Contracts.ActorEchoReq("after-heartbeat", 13, profile))
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(reply.actorId == actorId, "SM-D13 actor reply mismatch")
            ensure(reply.nodeRid == "session-a", "SM-D13 actor node mismatch")
            ensure(reply.value == "entry:after-heartbeat", "SM-D13 actor value mismatch")

            println("scenario SM-D13 passed")
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
