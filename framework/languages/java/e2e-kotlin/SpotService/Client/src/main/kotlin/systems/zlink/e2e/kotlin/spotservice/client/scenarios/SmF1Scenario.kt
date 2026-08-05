package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmF1Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        spots.sendState("room-a", "mixed-route-send")
        println("scenario SM-F1 passed")
    }
}
