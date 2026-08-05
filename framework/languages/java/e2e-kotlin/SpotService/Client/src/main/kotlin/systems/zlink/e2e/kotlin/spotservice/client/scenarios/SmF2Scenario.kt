package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmF2Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val reply = eventually {
            spots.requestState("room-a", "route-mesh")
        }
        ensure(reply.nodeRid == "play-a", "SM-F2 route mesh target mismatch")
        println("scenario SM-F2 passed")
    }
}
