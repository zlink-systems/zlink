package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal object SmF4Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        expectFailure {
            spots.requestState("missing-route", "missing-route", timeoutMilliseconds = 300)
        }
        println("scenario SM-F4-missing-route passed")
    }
}
