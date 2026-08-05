package systems.zlink.e2e.kotlin.registrationcodec

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

object Env {
    private val values = linkedMapOf<String, String>()

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

    fun get(name: String, fallback: String = ""): String {
        val value = values[name]
        return if (value.isNullOrBlank()) fallback else value
    }

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
