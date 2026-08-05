package systems.zlink.e2e.kotlin.discoveryregistryha

class CliOptions private constructor(
    private val values: Map<String, String>,
) {
    companion object {
        @JvmStatic
        fun parse(args: Array<String>): CliOptions {
            val values = linkedMapOf<String, String>()
            var index = 0
            while (index < args.size) {
                val key = args[index]
                require(key.startsWith("--")) { "Unexpected argument '$key'." }
                require(index + 1 < args.size) { "Missing value for '$key'." }
                values[key] = args[index + 1]
                index += 2
            }
            return CliOptions(values)
        }
    }

    fun get(name: String): String = get(name, "")

    fun get(name: String, fallback: String): String {
        val value = values[name]
        return if (value.isNullOrBlank()) fallback else value
    }

    fun csv(name: String): List<String> {
        val value = get(name)
        if (value.isBlank()) {
            return emptyList()
        }
        return value.split(",")
            .map(String::trim)
            .filter(String::isNotBlank)
    }
}
