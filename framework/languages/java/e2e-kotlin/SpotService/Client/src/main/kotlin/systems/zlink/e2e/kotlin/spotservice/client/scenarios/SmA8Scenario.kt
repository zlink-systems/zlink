package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver
import systems.zlink.e2e.kotlin.spotservice.client.support.waitForEvidence

internal object SmA8Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        eventually {
            spots.requestState("room-a", "worker-start-long")
        }
        val followUp = eventually {
            spots.requestState("room-a", "worker-follow-up")
        }
        ensure(followUp.value.contains("worker-follow-up"), "SM-A8 follow-up state was not applied")
        val evidenceEndpoint = Env.get("e2e.http.a.endpoint")
        waitForEvidence(evidenceEndpoint, "WorkerFollowUpBeforeComplete")
        waitForEvidence(evidenceEndpoint, "WorkerCompleted")
        println("scenario SM-A8 passed")
    }
}
