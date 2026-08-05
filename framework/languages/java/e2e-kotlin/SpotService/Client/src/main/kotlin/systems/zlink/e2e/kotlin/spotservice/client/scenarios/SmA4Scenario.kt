package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmA4Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val first = eventually {
            spots.requestState("room-a", "owner-stable-1")
        }
        val second = eventually {
            spots.requestState("room-a", "owner-stable-2")
        }
        ensure(first.nodeRid == "play-a", "SM-A4 first owner mismatch")
        ensure(second.nodeRid == first.nodeRid, "SM-A4 owner was not stable")
        println("scenario SM-A4 passed")
    }
}
