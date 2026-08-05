package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually

internal object SmF5Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val before = spots.routePing("play-a", "sm-f5-before")
        ensure(before.nodeRid == "play-a", "SM-F5 pre-routing channel target mismatch")
        ensure(before.value == "route:sm-f5-before", "SM-F5 pre-routing channel reply mismatch")

        val spotRid = "room-a"
        val routed = eventually {
            spots.requestState(spotRid, "add")
        }
        ensure(routed.spotRid == spotRid, "SM-F5 spot routing target mismatch")
        ensure(routed.nodeRid == "play-a", "SM-F5 spot routing node mismatch")

        val command = spots.sendState(spotRid, "sm-f5-command")
        ensure(command.accepted, "SM-F5 spot routing command was not accepted")

        val after = spots.routePing("play-a", "sm-f5-after")
        ensure(after.nodeRid == "play-a", "SM-F5 post-routing channel target mismatch")
        ensure(after.value == "route:sm-f5-after", "SM-F5 post-routing channel reply mismatch")

        println("scenario SM-F5 passed")
    }
}
