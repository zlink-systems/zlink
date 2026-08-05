package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmD1Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        ensure(context.entryPush.requestSeq == 1, "SM-D1 entry push request sequence mismatch")
        ensure(context.entryPush.value == "push:entry-echo", "SM-D1 entry push mismatch")
        ensure(context.userPush1.requestSeq == 2, "SM-D1 user push request sequence mismatch")
        ensure(context.userPush1.value == "push:user-echo-1", "SM-D1 user push mismatch")

        println("scenario SM-D1 passed")
    }
}
