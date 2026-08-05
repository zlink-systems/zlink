package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmF3Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val routeReply = spots.routePing("play-a", "route-mesh-normal")
        ensure(routeReply.nodeRid == "play-a", "SM-F3 route-channel target node mismatch")
        ensure(routeReply.value == "route:route-mesh-normal", "SM-F3 route-channel reply mismatch")
        println("scenario SM-F3 passed")
    }
}
