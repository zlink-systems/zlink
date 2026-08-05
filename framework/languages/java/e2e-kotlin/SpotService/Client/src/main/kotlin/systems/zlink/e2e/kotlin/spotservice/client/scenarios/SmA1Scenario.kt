package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmA1Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val first = eventually {
            spots.requestState("room-a", "a1")
        }
        ensure(first.spotRid == "room-a", "SM-A1 wrong spot rid")
        ensure(first.nodeRid == "play-a", "SM-A1 wrong owner node")
        println("scenario SM-A1 passed")
    }
}
