package systems.zlink.e2e.kotlin.observabilityops.a5.server

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

object Env {
    private val values = Properties()

    fun configure(args: Array<String>) {
        require(args.size == 2 && args[0] == "--e2e-config") {
            "Usage: observability-ops-kotlin-a5-server --e2e-config <path>"
        }
        Files.newInputStream(Path.of(args[1])).use { values.load(it) }
    }

    fun get(name: String, default: String? = null): String =
        values.getProperty(name, default)
            ?: error("Missing required property $name")
}
