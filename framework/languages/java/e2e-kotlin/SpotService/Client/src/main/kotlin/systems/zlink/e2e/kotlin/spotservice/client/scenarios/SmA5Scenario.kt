package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postAdmin
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmA5Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val spotRid = "spot-sm-a5-${System.nanoTime()}"
        val created = postJson(
            Env.get("e2e.http.a.endpoint"),
            "/spot/create",
            Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes::class.java
        )
        ensure(created.spotRid == spotRid, "SM-A5 did not create the requested spot")
        ensure(created.nodeRid == "play-a", "SM-A5 created spot on the wrong node")

        val ready = spots.requestState(spotRid, "noop")
        ensure(ready.spotRid == spotRid && ready.nodeRid == "play-a", "SM-A5 spot route was not ready")

        val stageReply = spots.requestStage(spotRid, "sm-a5-stage", "stage-add")
        ensure(stageReply.spotRid == spotRid, "SM-A5 stage request reached the wrong spot")
        ensure(stageReply.nodeRid == "play-a", "SM-A5 stage request reached the wrong node")
        ensure(stageReply.value.contains("stage-add"), "SM-A5 stage request did not update state")

        val timer = spots.startStageTimer(spotRid, "sm-a5-stage-timer", 50)
        ensure(timer.spotRid == spotRid && timer.started, "SM-A5 stage timer was not started")

        val evidence = postJson(
            Env.get("e2e.http.a.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf(
                    "SpotInitialized",
                    spotRid,
                    "StageRequest",
                    "sm-a5-stage",
                    "StageTimer",
                    "sm-a5-stage-timer"
                )
            ),
            Contracts.EvidenceSnapshot::class.java
        )
        ensure(
            evidence.entries.any { entry ->
                entry.marker == "StageRequest" &&
                    entry.spotRid == spotRid &&
                    entry.value?.contains("sm-a5-stage") == true
            },
            "SM-A5 stage evidence missing"
        )

        val closed = postAdmin(Env.get("e2e.http.a.endpoint"), "/admin/close?rid=$spotRid")
        ensure(closed.contains("\"closed\":true"), "SM-A5 did not close the stage spot")
        println("scenario SM-A5 passed")
    }
}
