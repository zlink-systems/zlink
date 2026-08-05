package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

object Env {
    private val values = linkedMapOf<String, String>()

    @JvmStatic
    @Synchronized
    fun configure(args: Array<out String>) {
        values.clear()
        val path = configPath(args) ?: return
        val properties = Properties()
        Files.newInputStream(Path.of(path)).use { properties.load(it) }
        for (name in properties.stringPropertyNames()) {
            values[name] = properties.getProperty(name)
        }
    }

    @JvmStatic
    fun get(name: String): String =
        values[name] ?: throw IllegalStateException("missing config value $name")

    @JvmStatic
    fun get(name: String, defaultValue: String): String =
        values[name] ?: defaultValue

    @JvmStatic
    fun applicationArgs(args: Array<out String>): Array<String> = withoutConfig(args)

    private fun configPath(args: Array<out String>): String? {
        for (index in args.indices) {
            val argument = args[index]
            if (argument == "--e2e-config" && index + 1 < args.size) {
                return args[index + 1]
            }
            if (argument.startsWith("--e2e-config=")) {
                return argument.substringAfter('=', "")
            }
        }
        return null
    }

    private fun withoutConfig(args: Array<out String>): Array<String> {
        val result = mutableListOf<String>()
        var index = 0
        while (index < args.size) {
            val argument = args[index]
            if (argument == "--e2e-config") {
                index += 2
            } else if (argument.startsWith("--e2e-config=")) {
                index++
            } else {
                result += argument
                index++
            }
        }
        return result.toTypedArray()
    }

}
