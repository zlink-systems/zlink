package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmG2Scenario {
    suspend fun run() {
        val playA = Env.get("e2e.http.a.endpoint")
        val playB = Env.get("e2e.http.b.endpoint")
        val key = "key-sm-g2-${UUID.randomUUID().toString().replace("-", "")}"
        val firstSpot = "spot-$key-a"
        val secondSpot = "spot-$key-b"

        val firstCreated = create(playA, firstSpot)
        val firstReply = requestState(playA, firstSpot, "owner-remap-first")
        val firstEvidence = waitEvidence(playA, "StateReq|play-a|$firstSpot|owner-remap-first")

        val secondCreated = create(playB, secondSpot)
        val secondReply = requestState(playB, secondSpot, "owner-remap-second")
        val secondEvidence = waitEvidence(playB, "StateReq|play-b|$secondSpot|owner-remap-second")

        ensure(firstCreated.nodeRid == "play-a", "SM-G2 first owner create node mismatch")
        ensure(secondCreated.nodeRid == "play-b", "SM-G2 remapped owner create node mismatch")
        ensure(firstReply.nodeRid == "play-a", "SM-G2 first owner request node mismatch")
        ensure(secondReply.nodeRid == "play-b", "SM-G2 remapped owner request node mismatch")
        ensure(!containsSpotEvidence(firstEvidence, secondSpot), "SM-G2 remapped owner leaked to play-a")
        ensure(!containsSpotEvidence(secondEvidence, firstSpot), "SM-G2 first owner leaked to play-b")

        println("scenario SM-G2 passed")
    }

    private fun create(endpoint: String, spotRid: String): Contracts.CreateSpotRes =
        postJson(
            endpoint,
            "/spot/create",
            Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes::class.java
        )

    private fun requestState(endpoint: String, spotRid: String, op: String): Contracts.StateRes =
        postJson(
            endpoint,
            "/spot/state/request",
            Contracts.SpotStateRouteReq(spotRid, op),
            Contracts.StateRes::class.java
        )

    private fun waitEvidence(endpoint: String, expected: String): Contracts.EvidenceSnapshot =
        postJson(
            endpoint,
            "/evidence/wait",
            Contracts.EvidenceWaitReq(listOf(expected), 10_000),
            Contracts.EvidenceSnapshot::class.java
        )

    private fun containsSpotEvidence(evidence: Contracts.EvidenceSnapshot, spotRid: String): Boolean =
        evidence.entries.any { it.spotRid == spotRid || it.value?.contains(spotRid) == true }
}
