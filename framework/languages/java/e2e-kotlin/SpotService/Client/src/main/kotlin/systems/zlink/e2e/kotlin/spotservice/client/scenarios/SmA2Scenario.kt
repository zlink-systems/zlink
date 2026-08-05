package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmA2Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val second = eventually {
            spots.requestState("room-a", "a2")
        }
        ensure(
            second.value.contains("a1") && second.value.contains("a2"),
            "SM-A2 state did not accumulate",
        )
        println("scenario SM-A2 passed")
    }
}
