package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

internal object SmD14Scenario {
    suspend fun run() {
        val endpoint = Env.get("e2e.tls.stream.a.endpoint")
        val strict = createStreamConnector(
            endpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            Int.MAX_VALUE,
            false
        )
        var strictTlsRejected = false
        try {
            strict.connect().await()
        } catch (_: Exception) {
            strictTlsRejected = true
        } finally {
            closeQuietly(strict)
        }
        ensure(strictTlsRejected, "SM-D14 expected strict TLS validation to reject self-signed certificate")

        val actorId = "actor-sm-d14-tls"
        val profile = Contracts.ActorProfile("Stream TLS", 14, listOf("tls"))
        val tls = createStreamConnector(
            endpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            Int.MAX_VALUE,
            true
        )
        try {
            tls.connect().await()
            val auth = tls
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D14 TLS auth actor mismatch")
            ensure(auth.nodeRid == "play-a", "SM-D14 TLS auth node mismatch")

            val (reply, notify) = coroutineScope {
                val pushed = async(start = CoroutineStart.UNDISPATCHED) {
                    tls.waitFor<Contracts.ActorPushNotify>().await()
                }
                val reply = tls
                    .request(Contracts.ActorEchoReq("tls-push", 14, profile))
                    .metadata("actor-id", actorId)
                    .awaitReply<Contracts.ActorEchoRes>()
                reply to pushed.await().payload()
            }
            ensure(reply.actorId == actorId, "SM-D14 TLS actor reply mismatch")
            ensure(reply.nodeRid == "play-a", "SM-D14 TLS actor node mismatch")
            ensure(notify.actorId == actorId, "SM-D14 TLS push actor mismatch")
            ensure(notify.value == "push:tls-push", "SM-D14 TLS push payload mismatch")

            println("scenario SM-D14 passed")
        } finally {
            closeQuietly(tls)
        }
    }

    private suspend fun closeQuietly(connector: ZLinkKotlinStreamConnector) {
        try {
            connector.close().await()
        } catch (_: Exception) {
        }
    }
}
