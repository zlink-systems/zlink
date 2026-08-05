package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson
import systems.zlink.e2e.kotlin.spotservice.client.support.postJsonArray

internal object SmQ9Scenario {
    suspend fun run() {
        val multiA = Env.get("e2e.multi.http.a.endpoint")
        val multiB = Env.get("e2e.multi.http.b.endpoint")
        val suffix = UUID.randomUUID().toString().replace("-", "")
        val spotA = "spot-sm-q9-a-$suffix"
        val spotB = "spot-sm-q9-b-$suffix"

        val createdA = create(multiA, spotA, 0)
        val firstA = requestState(multiA, spotA, 11)
        val directA = requestState(multiA, spotA, 0)
        val evidenceA = waitEvidence(
            multiA,
            "multi-state-request|node=${Contracts.MULTI_SPOT_NODE_A}|spot=$spotA|value=11"
        )

        val createdB = create(multiB, spotB, 0)
        val firstB = requestState(multiB, spotB, 17)
        val directB = requestState(multiB, spotB, 0)
        val evidenceB = waitEvidence(
            multiB,
            "multi-state-request|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$spotB|value=17"
        )

        ensure(createdA.nodeRid == Contracts.MULTI_SPOT_NODE_A, "SM-Q9 node A create reply node mismatch")
        ensure(firstA.value == 11, "SM-Q9 node A route-to-spot reply value mismatch")
        ensure(directA.spotRid == spotA, "SM-Q9 node A direct spot reply target mismatch")
        ensure(directA.nodeRid == Contracts.MULTI_SPOT_NODE_A, "SM-Q9 node A direct spot reply node mismatch")
        ensure(directA.value == 11, "SM-Q9 node A direct spot reply value mismatch")
        ensure(evidenceA.count { it == "multi-state-request|node=${Contracts.MULTI_SPOT_NODE_A}|spot=$spotA|value=11" } >= 2,
            "SM-Q9 node A did not process both route-to-spot requests")

        ensure(createdB.nodeRid == Contracts.MULTI_SPOT_NODE_B, "SM-Q9 node B create reply node mismatch")
        ensure(firstB.value == 17, "SM-Q9 node B route-to-spot reply value mismatch")
        ensure(directB.spotRid == spotB, "SM-Q9 node B direct spot reply target mismatch")
        ensure(directB.nodeRid == Contracts.MULTI_SPOT_NODE_B, "SM-Q9 node B direct spot reply node mismatch")
        ensure(directB.value == 17, "SM-Q9 node B direct spot reply value mismatch")
        ensure(evidenceB.count { it == "multi-state-request|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$spotB|value=17" } >= 2,
            "SM-Q9 node B did not process both route-to-spot requests")

        println("scenario SM-Q9 passed")
    }

    private fun create(
        endpoint: String,
        spotRid: String,
        delta: Int
    ): Contracts.MultiNodeCreateSpotRes =
        postJson(
            endpoint,
            "/spot/create-local",
            Contracts.MultiNodeCreateSpotReq(spotRid, delta),
            Contracts.MultiNodeCreateSpotRes::class.java
        )

    private fun requestState(
        endpoint: String,
        spotRid: String,
        delta: Int
    ): Contracts.MultiNodeStateRes =
        postJson(
            endpoint,
            "/spot/state/request",
            Contracts.MultiNodeStateRouteReq(spotRid, delta),
            Contracts.MultiNodeStateRes::class.java
        )

    private fun waitEvidence(
        endpoint: String,
        expected: String
    ): List<String> =
        postJsonArray(
            endpoint,
            "/evidence/wait",
            Contracts.EvidenceWaitReq(listOf(expected), 10_000)
        )
}
