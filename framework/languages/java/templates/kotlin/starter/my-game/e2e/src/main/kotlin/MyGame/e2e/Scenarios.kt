package MyGame.e2e

import MyGame.*
import java.net.URI
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

// Add application scenarios to Scenario and zlink.structure.json together.
suspend fun runScenario(scenario: Scenario, nodes: Nodes) =
    when (scenario) {
        Scenario.CONNECT -> connect(nodes)
    }

private suspend fun connect(nodes: Nodes) {
    val connector =
        ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI(nodes.endpoint))
        )
    withCleanup(cleanup = { connector.kotlin().close().await() }) {
        connector.kotlin().connect().await()
    }
    nodes.drain()
}
