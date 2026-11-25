package MyGame.e2e

import MyGame.*
import java.net.URI
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

// Add application scenarios to Scenario and zlink.structure.json together.
suspend fun runScenario(scenario: Scenario, target: ScenarioTarget) =
    when (scenario) {
        Scenario.CONNECT -> connect(target)
    }

private suspend fun connect(target: ScenarioTarget) {
    val connector =
        ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI(target.endpoint))
        )
    withCleanup(cleanup = { connector.kotlin().close().await() }) {
        connector.kotlin().connect().await()
    }
    target.deployment?.drain()
}
