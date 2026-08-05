package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.util.UUID
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector

internal object SmG3Scenario {
    suspend fun run() {
        val connectors = mutableListOf<ZLinkKotlinStreamConnector>()
        val actorIds = mutableListOf<String>()
        val key = UUID.randomUUID().toString().replace("-", "")
        val spotRid = "spot-sm-g3-$key"
        val profile = Contracts.ActorProfile("Race", 3, listOf("join", "leave"))
        try {
            createSpot(spotRid)

            repeat(2) { index ->
                val actorId = "actor-sm-g3-$key-$index"
                val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
                connectors += connector
                actorIds += actorId
                connector.connect().await()

                val auth = connector
                    .request(Contracts.ActorAuthReq(actorId, profile))
                    .awaitReply<Contracts.ActorAuthRes>()
                ensure(auth.actorId == actorId, "SM-G3 auth actor mismatch")

                val joined = connector
                    .request(Contracts.ActorJoinReq(spotRid, profile, profile.tags))
                    .metadata("actor-id", actorId)
                    .awaitReply<Contracts.ActorJoinRes>()
                ensure(joined.actorId == actorId, "SM-G3 join actor mismatch")
                ensure(joined.nodeRid == "play-a", "SM-G3 join node mismatch")
            }

            coroutineScope {
                actorIds.mapIndexed { index, actorId ->
                    val connector = connectors[index]
                    async {
                    val ping = connector
                        .request(Contracts.ActorEchoReq(actorId, 3, profile))
                        .metadata("actor-id", actorId)
                        .awaitReply<Contracts.ActorEchoRes>()
                    ensure(ping.actorId == actorId, "SM-G3 actor request target mismatch")
                    ensure(ping.nodeRid == "play-a", "SM-G3 actor request node mismatch")

                    val left = connector
                        .request(Contracts.LeaveActorReq(actorId))
                        .metadata("actor-id", actorId)
                        .awaitReply<Contracts.LeaveActorRes>()
                    ensure(left.actorId == actorId, "SM-G3 leave actor mismatch")
                    ensure(left.accepted, "SM-G3 leave was not accepted")
                    }
                }.awaitAll()
            }

            val expected = actorIds.flatMap { actorId ->
                listOf(
                    "ActorUserJoined|play-a|$spotRid|$actorId",
                    "ActorUserLeft|play-a|$spotRid|$actorId"
                )
            }
            val evidence = waitEvidence(expected)
            actorIds.forEach { actorId ->
                val joinedCount = countActorEvidence(evidence, "ActorUserJoined", spotRid, actorId)
                val leftCount = countActorEvidence(evidence, "ActorUserLeft", spotRid, actorId)
                ensure(joinedCount == 1, "SM-G3 join evidence count mismatch for $actorId")
                ensure(leftCount == 1, "SM-G3 leave evidence count mismatch for $actorId")
            }

            println("scenario SM-G3 passed")
        } finally {
            connectors.forEach { connector ->
                try {
                    connector.close().await()
                } catch (_: Exception) {
                }
            }
        }
    }

    private fun createSpot(spotRid: String): Contracts.CreateSpotRes =
        postJson(
            Env.get("e2e.http.a.endpoint"),
            "/spot/create",
            Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes::class.java
        )

    private fun waitEvidence(expected: List<String>): Contracts.EvidenceSnapshot =
        postJson(
            Env.get("e2e.http.a.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(expected, 10_000),
            Contracts.EvidenceSnapshot::class.java
        )

    private fun countActorEvidence(
        evidence: Contracts.EvidenceSnapshot,
        marker: String,
        spotRid: String,
        actorId: String
    ): Int =
        evidence.entries.count { entry ->
            entry.marker == marker &&
                entry.spotRid == spotRid &&
                entry.value?.startsWith(actorId) == true
        }
}
