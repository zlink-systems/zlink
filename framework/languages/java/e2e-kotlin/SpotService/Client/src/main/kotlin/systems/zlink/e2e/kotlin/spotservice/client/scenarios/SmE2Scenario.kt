package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.waitForEvidence

internal object SmE2Scenario {
    suspend fun run() {
        waitForEvidence(Env.get("e2e.http.a.endpoint"), "SpotTimer")
        println("scenario SM-E2 passed")
    }
}
