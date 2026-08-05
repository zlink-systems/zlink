package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postAdmin
import systems.zlink.e2e.kotlin.spotservice.client.support.waitForEvidence

internal object SmA7Scenario {
    suspend fun run() {
        val endpoint = Env.get("e2e.http.a.endpoint")
        val body = postAdmin(endpoint, "/admin/type-mismatch?rid=room-a")
        ensure(body.contains("\"mismatch\":true"), "SM-A7 spot type mismatch did not report mismatch=true")
        waitForEvidence(endpoint, "SpotTypeMismatch")
        waitForEvidence(endpoint, "SpotTypeMismatchStateOk")
        println("scenario SM-A7 passed")
    }
}
