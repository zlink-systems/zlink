package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmE1Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        expectFailure {
            spots.requestState("room-a", "missing", packetName = "MissingSpotPacket")
        }
        spots.sendState("room-a", "missing-send", packetName = "MissingSpotMsg")
        println("scenario SM-C1-negative passed")
        println("scenario SM-E1 passed")
    }
}
