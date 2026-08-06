package systems.zlink.e2e.kotlin.instancespot.shared

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

object Env {
    private val values = linkedMapOf<String, String>()

    @Synchronized
    fun configure(args: Array<out String>) {
        values.clear()
        val path = args.toList().windowed(2).firstOrNull { it[0] == "--e2e-config" }?.get(1)
            ?: args.firstOrNull { it.startsWith("--e2e-config=") }?.substringAfter('=')
            ?: return
        val properties = Properties()
        Files.newInputStream(Path.of(path)).use(properties::load)
        properties.stringPropertyNames().forEach { name -> values[name] = properties.getProperty(name) }
    }

    fun get(name: String, fallback: String = ""): String = values[name]?.takeIf { it.isNotBlank() } ?: fallback

    fun withoutConfig(args: Array<out String>): Array<String> {
        val result = mutableListOf<String>()
        var index = 0
        while (index < args.size) {
            when {
                args[index] == "--e2e-config" -> index += 2
                args[index].startsWith("--e2e-config=") -> index++
                else -> result += args[index++]
            }
        }
        return result.toTypedArray()
    }
}
