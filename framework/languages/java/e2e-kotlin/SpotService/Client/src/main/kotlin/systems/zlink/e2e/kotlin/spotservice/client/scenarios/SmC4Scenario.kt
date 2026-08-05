package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmC4Scenario {
    suspend fun run() {
        val gateway = Env.get("e2e.gateway.http.endpoint")
        val suffix = System.nanoTime().toString()
        val spotRid = "spot-sm-c4-$suffix"
        val marker = "sm-c4-gateway"

        val publish = postJson(
            gateway,
            "/spot/publish",
            mapOf("spotRid" to spotRid, "marker" to marker),
            Map::class.java
        )
        ensure(publish["scenario"] == "spot.sm-c4-publish", "SM-C4 publish operation mismatch")
        ensure(publish["rid"] == "gateway", "SM-C4 publisher was not the publish-only gateway")
        ensure(publish["spotRid"] == spotRid, "SM-C4 publish target spot mismatch")

        println("scenario SM-C4-gateway passed")
    }
}
