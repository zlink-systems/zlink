package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import kotlinx.coroutines.delay
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmD8Scenario {
    suspend fun run() {
        val actorId = "actor-sm-d8-reconnect"
        val profile = Contracts.ActorProfile("Reconnect", 8, listOf("reconnect"))
        val first = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            first.connect().await()
            val auth = first
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D8 initial auth actor mismatch")

            val pending = first
                .request(Contracts.SlowSessionReq("before-disconnect", 1000))
                .timeout(Duration.ofSeconds(10))
                .submit(Contracts.SlowSessionRes::class.java)
            delay(100)
            first.close().await()

            var pendingFailed = false
            try {
                pending.await()
            } catch (_: Exception) {
                pendingFailed = true
            }
            ensure(pendingFailed, "SM-D8 expected pending request to fail after stream disconnect")
        } finally {
            try {
                first.close().await()
            } catch (_: Exception) {
            }
        }

        postJson(
            Env.get("e2e.http.session.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf("ActorEntryDisconnected|session-a|entry|$actorId"),
                10_000,
            ),
            Contracts.EvidenceSnapshot::class.java,
        )

        val second = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            second.connect().await()
            val auth = second
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D8 reauth actor mismatch")

            val reply = second
                .request(Contracts.ActorEchoReq("after-reconnect", 8, profile))
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(reply.actorId == actorId, "SM-D8 reconnected actor mismatch")
            ensure(reply.nodeRid == "session-a", "SM-D8 reconnected node mismatch")
            ensure(reply.value == "entry:after-reconnect", "SM-D8 reconnected value mismatch")

            println("scenario SM-D8 passed")
        } finally {
            try {
                second.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
