package systems.zlink.e2e.kotlin.pubsub.client.Scenarios

import systems.zlink.e2e.kotlin.pubsub.client.Support.ScenarioContext

object MissingMessageNameScenario {
    fun run(context: ScenarioContext) {
        context.runMissingPacket()
    }
}
