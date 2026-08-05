package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmD11Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val actorId = "actor-sm-d11-mixed"
        val profile = Contracts.ActorProfile("Mixed", 11, listOf("stream", "channel"))
        val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            connector.connect().await()
            val auth = connector
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D11 auth actor mismatch")

            val streamReply = connector
                .request(Contracts.ActorEchoReq("stream-side", 11, profile))
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(streamReply.actorId == actorId, "SM-D11 stream request actor mismatch")
            ensure(streamReply.value == "entry:stream-side", "SM-D11 stream reply value mismatch")
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }

        val routeReply = spots.routePing("play-a", "channel-side")
        ensure(routeReply.nodeRid == "play-a", "SM-D11 channel request node mismatch")
        ensure(routeReply.value == "route:channel-side", "SM-D11 channel reply value mismatch")

        println("scenario SM-D11 passed")
    }
}
