package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmC1Scenario {
    suspend fun runSend(spots: SpotHttpDriver) {
        spots.sendState("room-a", "cmd-c1")
        println("scenario SM-C1-send passed")
    }

    suspend fun runTimeout(spots: SpotHttpDriver) {
        expectFailure {
            spots.requestSlow("room-a", "late", 100)
        }
        println("scenario SM-C1-timeout passed")
    }

    suspend fun runNormal(spots: SpotHttpDriver) {
        val after = eventually {
            spots.requestState("room-a", "after-timeout")
        }
        ensure(after.value.contains("after-timeout"), "SM-C1 post-timeout request failed")
        println("scenario SM-C1 passed")
        println("scenario SM-C1-normal passed")
    }
}
