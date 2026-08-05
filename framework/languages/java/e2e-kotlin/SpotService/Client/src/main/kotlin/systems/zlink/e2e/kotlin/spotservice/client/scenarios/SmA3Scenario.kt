package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmA3Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val roomA = eventually {
            spots.requestState("room-a", "owner-a")
        }
        ensure(roomA.nodeRid == "play-a", "SM-A3 room-a owner mismatch")
        println("scenario SM-A3 passed")
    }
}
