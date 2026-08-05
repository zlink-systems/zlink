package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmB8Scenario {
    suspend fun run() {
        val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            val actorId = "actor-sm-b8-destroy"
            val profile = Contracts.ActorProfile("Destroy", 8, listOf("destroy"))
            connector.connect().await()
            val auth = connector
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-B8 auth actor mismatch")

            val destroyed = connector
                .request(Contracts.DestroyActorReq(actorId))
                .awaitReply<Contracts.DestroyActorRes>()
            ensure(destroyed.destroyed && destroyed.actorId == actorId, "SM-B8 destroy reply mismatch")

            val evidence = postJson(
                Env.get("e2e.http.session.endpoint"),
                "/evidence/wait",
                Contracts.EvidenceWaitReq(
                    listOf("ActorDestroyed|session-a|entry|$actorId"),
                    10_000,
                ),
                Contracts.EvidenceSnapshot::class.java,
            )
            ensure(
                evidence.entries.none { it.marker == "ActorDestroyFailed" && it.value?.contains(actorId) == true },
                "SM-B8 actor destroy reported a failure",
            )

            expectFailure {
                connector
                    .request(Contracts.ActorEchoReq("after-destroy", 8, profile))
                    .timeout(Duration.ofMillis(500))
                    .awaitReply<Contracts.ActorEchoRes>()
            }

            println("scenario SM-B8 passed")
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
