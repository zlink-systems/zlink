package systems.zlink.samples.kotlin.zoneworld.client

import java.io.Reader
import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties
data class ClientOptions(
    val gatewayEndpoint: String,
    val opsEndpoint: String,
    val scenario: String,
) {
    companion object {
        fun load(args: Array<String>): ClientOptions {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: ZoneWorldClient --config <path>"
            }
            val properties = Properties()
            Files.newBufferedReader(Path.of(args[1])).use { reader: Reader -> properties.load(reader) }
            return ClientOptions(
                gatewayEndpoint = required(properties, "sample.gateway-endpoint"),
                opsEndpoint = required(properties, "sample.ops-endpoint"),
                scenario = properties.getProperty("sample.scenario", "full"),
            )
        }

        private fun required(properties: Properties, key: String): String =
            properties.getProperty(key)?.takeIf { it.isNotBlank() }
                ?: error("$key is required")
    }
}
