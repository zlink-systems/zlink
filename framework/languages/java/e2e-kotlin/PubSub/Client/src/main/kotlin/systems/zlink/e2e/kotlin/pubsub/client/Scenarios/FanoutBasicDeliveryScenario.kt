package systems.zlink.e2e.kotlin.pubsub.client.Scenarios

import systems.zlink.e2e.kotlin.pubsub.client.Support.ScenarioContext

object FanoutBasicDeliveryScenario {
    fun run(context: ScenarioContext) {
        context.runFanoutBasicDelivery()
    }
}
