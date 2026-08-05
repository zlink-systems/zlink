package systems.zlink.samples.kotlin.bingo.client.configuration

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

data class SampleTopology(
    val sessionAStreamEndpoint: String,
    val sessionBStreamEndpoint: String,
) {
    companion object {
    fun load(args: Array<String>): SampleTopology {
        require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
            "Usage: Client --config <path>"
        }
        val properties = Properties().also { values ->
            Files.newBufferedReader(Path.of(args[1])).use(values::load)
        }
        return SampleTopology(
            required(properties, "sample.session-a-stream-endpoint"),
            required(properties, "sample.session-b-stream-endpoint"),
        )
    }

    private fun required(properties: Properties, name: String): String =
        requireNotNull(properties.getProperty(name)?.takeIf(String::isNotBlank)) { "$name is required" }
    }
}
