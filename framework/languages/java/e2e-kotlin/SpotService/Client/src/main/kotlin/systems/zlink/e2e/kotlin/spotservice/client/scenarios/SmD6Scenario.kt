package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.supervisorScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmD6Scenario {
    suspend fun run() {
        val bound = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        val shadow = createStreamConnector(Env.get("e2e.stream.b.endpoint"))
        try {
            val boundProfile = Contracts.ActorProfile("Bound", 6, listOf("bound"))
            val shadowProfile = Contracts.ActorProfile("Shadow", 6, listOf("shadow"))
            bound.connect().await()
            bound
                .request(Contracts.ActorAuthReq("actor-sm-d6", boundProfile))
                .awaitReply<Contracts.ActorAuthRes>()
            shadow.connect().await()
            shadow
                .request(Contracts.ActorAuthReq("actor-sm-d6-shadow", shadowProfile))
                .awaitReply<Contracts.ActorAuthRes>()

            val (reply, notify) = supervisorScope {
                val shadowPush = async(start = CoroutineStart.UNDISPATCHED) {
                    shadow.expectNone<Contracts.ActorPushNotify>("ActorPushNotify")
                        .within(Duration.ofMillis(400))
                        .await()
                }
                val boundPush = async(start = CoroutineStart.UNDISPATCHED) {
                    bound.waitFor<Contracts.ActorPushNotify>().await()
                }
                val reply = bound
                    .request(Contracts.ActorEchoReq("push-bound-only", 20, boundProfile))
                    .awaitReply<Contracts.ActorEchoRes>()
                val notify = boundPush.await().payload()
                shadowPush.await()
                reply to notify
            }

            ensure(reply.actorId == "actor-sm-d6", "SM-D6 reply actor mismatch")
            ensure(notify.actorId == "actor-sm-d6", "SM-D6 push actor mismatch")
            ensure(notify.value == "push:push-bound-only", "SM-D6 push value mismatch")
            println("scenario SM-D6 passed")
        } finally {
            try {
                bound.close().await()
            } catch (_: Exception) {
            }
            try {
                shadow.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
