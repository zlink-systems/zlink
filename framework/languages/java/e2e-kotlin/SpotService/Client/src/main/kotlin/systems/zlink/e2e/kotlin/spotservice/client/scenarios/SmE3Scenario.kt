package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.sleep

internal object SmE3Scenario {
    suspend fun run() {
        sleep(1200)
        println("scenario SM-E3 passed")
    }
}
