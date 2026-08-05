package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson
import systems.zlink.e2e.kotlin.spotservice.client.support.postJsonArray

internal object SmF6Scenario {
    suspend fun run() {
        val multiA = Env.get("e2e.multi.http.a.endpoint")
        val multiB = Env.get("e2e.multi.http.b.endpoint")
        val suffix = UUID.randomUUID().toString().replace("-", "")
        val sourceSpotRid = "spot-sm-f6-source-$suffix"
        val targetSpotRid = "spot-sm-f6-target-$suffix"
        val actorId = "actor-sm-f6-$suffix"
        val marker = "sm-f6-$suffix"

        val target = postJson(
            multiB,
            "/spot/create-local",
            Contracts.MultiNodeCreateSpotReq(targetSpotRid, 0),
            Contracts.MultiNodeCreateSpotRes::class.java
        )
        ensure(target.nodeRid == Contracts.MULTI_SPOT_NODE_B, "SM-F6 target spot was not created on node B")

        val mesh = postJson(
            multiA,
            "/spot/spot-only/request-send",
            Contracts.SpotOnlyMeshReq(sourceSpotRid, targetSpotRid, marker),
            Contracts.SpotOnlyMeshRes::class.java
        )
        ensure(mesh.targetSpotRid == targetSpotRid, "SM-F6 request target mismatch")
        ensure(mesh.targetValue == 7, "SM-F6 target request value mismatch")

        val join = postJson(
            multiA,
            "/actor/spot-only-join",
            Contracts.SpotOnlyJoinReq(targetSpotRid, actorId, marker),
            Contracts.SpotOnlyJoinRes::class.java
        )
        ensure(join.accepted, "SM-F6 spot-only actor join was rejected")
        ensure(join.actorId == actorId, "SM-F6 actor join id mismatch")

        val evidence = postJsonArray(
            multiB,
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf(
                    "multi-state-request|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$targetSpotRid|value=7",
                    "spot-state-command|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$targetSpotRid|marker=sm-f6-send-$marker",
                    "spot-actor-joined|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$targetSpotRid|actor=$actorId",
                ),
                10_000
            )
        )
        ensure(
            evidence.any { it.contains("spot-actor-joined|node=${Contracts.MULTI_SPOT_NODE_B}|spot=$targetSpotRid|actor=$actorId") },
            "SM-F6 remote actor join evidence did not appear on node B"
        )
        println("scenario SM-F6 passed")
    }
}
