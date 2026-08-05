package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure

internal object SmD7Scenario {
    suspend fun run() {
        val preAuth = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            preAuth.connect().await()
            expectFailure {
                preAuth
                    .request(
                        Contracts.ActorEchoReq(
                            "pre-auth-dispatch",
                            7,
                            Contracts.ActorProfile("PreAuth", 7, listOf("pre-auth")),
                        )
                    )
                    .timeout(Duration.ofMillis(500))
                    .awaitReply<Contracts.ActorEchoRes>()
            }
        } finally {
            try {
                preAuth.close().await()
            } catch (_: Exception) {
            }
        }

        val authenticated = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            val profile = Contracts.ActorProfile("Auth Ok", 7, listOf("auth"))
            authenticated.connect().await()
            val auth = authenticated
                .request(Contracts.ActorAuthReq("actor-sm-d7", profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == "actor-sm-d7", "SM-D7 auth actor mismatch")

            val (reply, notify) = coroutineScope {
                val push = async(start = CoroutineStart.UNDISPATCHED) {
                    authenticated.waitFor<Contracts.ActorPushNotify>().await()
                }
                val reply = authenticated
                    .request(Contracts.ActorEchoReq("auth-ok", 7, profile))
                    .awaitReply<Contracts.ActorEchoRes>()
                reply to push.await().payload()
            }

            ensure(reply.actorId == "actor-sm-d7", "SM-D7 relay actor mismatch")
            ensure(reply.value == "entry:auth-ok", "SM-D7 relay value mismatch")
            ensure(notify.actorId == "actor-sm-d7", "SM-D7 push actor mismatch")
            ensure(notify.value == "push:auth-ok", "SM-D7 push value mismatch")

            println("scenario SM-D7 passed")
        } finally {
            try {
                authenticated.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
