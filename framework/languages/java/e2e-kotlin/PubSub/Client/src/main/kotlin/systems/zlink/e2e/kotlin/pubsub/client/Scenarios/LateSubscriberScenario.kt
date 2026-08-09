package Scenarios

import systems.zlink.e2e.kotlin.pubsub.client.Scenarios
import systems.zlink.e2e.kotlin.pubsub.client.Support
import systems.zlink.e2e.kotlin.pubsub.client.Support.ScenarioContext

object LateSubscriberScenario {
    fun run(context: ScenarioContext) {
        context.runLateSubscriber()
    }
}
