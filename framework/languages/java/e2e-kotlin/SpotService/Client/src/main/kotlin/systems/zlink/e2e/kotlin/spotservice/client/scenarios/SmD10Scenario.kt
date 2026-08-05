package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

internal object SmD10Scenario {
    suspend fun run() {
        val congestedActorId = "actor-sm-d10-congested"
        val isolatedActorId = "actor-sm-d10-isolated"
        val congested = createStreamConnector(
            Env.get("e2e.stream.a.endpoint"),
            ZLinkStreamDispatchMode.MANUAL,
            1,
        )
        val isolated = createStreamConnector(Env.get("e2e.stream.b.endpoint"))
        try {
            val congestedProfile = Contracts.ActorProfile("Backpressure", 10, listOf("congested"))
            val isolatedProfile = Contracts.ActorProfile("Backpressure Peer", 10, listOf("isolated"))
            congested.connect().await()
            congested
                .request(Contracts.ActorAuthReq(congestedActorId, congestedProfile))
                .awaitReply<Contracts.ActorAuthRes>()
            isolated.connect().await()
            isolated
                .request(Contracts.ActorAuthReq(isolatedActorId, isolatedProfile))
                .awaitReply<Contracts.ActorAuthRes>()

            val retained = coroutineScope {
                val retainedPush = async(start = CoroutineStart.UNDISPATCHED) {
                    congested.waitFor<Contracts.ActorPushNotify>().await()
                }
                for (index in 0 until 8) {
                    val reply = congested
                        .request(Contracts.ActorEchoReq("burst-$index", 10, congestedProfile))
                        .awaitReply<Contracts.ActorEchoRes>()
                    ensure(reply.actorId == congestedActorId, "SM-D10 congested reply actor mismatch")
                }
                ensure(congested.receivedCount("ActorPushNotify") <= 1, "SM-D10 congested queue retained too many pushes")
                congested.dispatch().await()
                retainedPush.await().payload()
            }
            ensure(retained.actorId == congestedActorId, "SM-D10 retained push actor mismatch")
            ensure(retained.value == "push:burst-7", "SM-D10 expected newest congested push to be retained")

            val stillAlive = congested
                .request(Contracts.ActorEchoReq("after-backpressure", 10, congestedProfile))
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(stillAlive.actorId == congestedActorId, "SM-D10 congested session stopped routing")
            ensure(stillAlive.value == "entry:after-backpressure", "SM-D10 congested session reply mismatch")
            congested.dispatch().await()

            val (isolatedReply, isolatedNotify) = coroutineScope {
                val isolatedPush = async(start = CoroutineStart.UNDISPATCHED) {
                    isolated.waitFor<Contracts.ActorPushNotify>().await()
                }
                val isolatedReply = isolated
                    .request(Contracts.ActorEchoReq("isolated-push", 10, isolatedProfile))
                    .awaitReply<Contracts.ActorEchoRes>()
                isolatedReply to isolatedPush.await().payload()
            }
            ensure(isolatedReply.actorId == isolatedActorId, "SM-D10 isolated reply actor mismatch")
            ensure(isolatedNotify.actorId == isolatedActorId, "SM-D10 isolated push actor mismatch")
            ensure(isolatedNotify.value == "push:isolated-push", "SM-D10 isolated session push mismatch")

            println("scenario SM-D10 passed")
        } finally {
            try {
                congested.close().await()
            } catch (_: Exception) {
            }
            try {
                isolated.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
