package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmC2Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val reply = eventually {
            spots.requestOutbound("room-a", "c2")
        }
        ensure(reply.spotRid == "room-a", "SM-C2 wrong source spot")
        ensure(reply.nodeRid == "play-a", "SM-C2 wrong source node")
        ensure(reply.channelReply == "c2", "SM-C2 channel request reply mismatch")
        println("scenario SM-C2 passed")
    }
}
