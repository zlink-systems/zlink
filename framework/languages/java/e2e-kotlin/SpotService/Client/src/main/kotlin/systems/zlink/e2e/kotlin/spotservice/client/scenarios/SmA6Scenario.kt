package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postAdmin

internal object SmA6Scenario {
    suspend fun run() {
        val rid = "room-b-lifecycle-${System.nanoTime()}"
        val created = postAdmin(Env.get("e2e.http.b.endpoint"), "/admin/create-user?rid=$rid")
        ensure(created.contains("\"created\":true"), "SM-A6 lifecycle spot create did not report created=true")
        val body = postAdmin(Env.get("e2e.http.b.endpoint"), "/admin/close?rid=$rid")
        ensure(body.contains("\"closed\":true"), "SM-A6 spot close did not report closed=true")
        println("scenario SM-A6 passed")
    }
}
