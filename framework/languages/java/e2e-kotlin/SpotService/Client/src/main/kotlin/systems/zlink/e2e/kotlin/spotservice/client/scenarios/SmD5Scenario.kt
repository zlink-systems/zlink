package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmD5Scenario {
    suspend fun run() {
        val actorId = "actor-sm-d5-notified-" + UUID.randomUUID().toString().replace("-", "")
        val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            val profile = Contracts.ActorProfile("Disconnect", 5, listOf("disconnect"))
            connector.connect().await()
            val auth = connector
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-D5 auth actor mismatch")
            val joined = connector
                .request(Contracts.ActorJoinReq("actor-room-a", profile, profile.tags))
                .metadata("actor-id", actorId)
                .awaitReply<Contracts.ActorJoinRes>()
            ensure(joined.actorId == actorId, "SM-D5 join actor mismatch")
            connector.close().await()
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }

        val evidence = postJson(
            Env.get("e2e.http.session.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf(
                    "ActorUserDisconnected|session-a|actor-room-a|$actorId",
                ),
                10_000,
            ),
            Contracts.EvidenceSnapshot::class.java,
        )
        ensure(
            evidence.entries.any { it.marker == "ActorUserDisconnected" && it.value == actorId },
            "SM-D5 expected selected actor disconnect callback evidence",
        )

        println("scenario SM-D5 passed")
    }
}
