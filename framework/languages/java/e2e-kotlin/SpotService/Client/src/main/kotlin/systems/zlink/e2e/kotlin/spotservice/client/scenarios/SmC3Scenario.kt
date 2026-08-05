package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmC3Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val requestReply = eventually {
            spots.requestState("room-a", "c3-source")
        }
        ensure(requestReply.nodeRid == "play-a", "SM-C3 source spot owner mismatch")
        val ack = eventually {
            spots.sendOutbound("room-b", "c3-send")
        }
        ensure(ack.accepted, "SM-C3 spot-to-spot command was not accepted")
        println("scenario SM-C3 passed")
    }
}
