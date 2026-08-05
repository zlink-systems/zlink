package systems.zlink.e2e.kotlin.registrationcodec.invalidduplicate.configuration

data class ServerOptions(
    val serverEndpoint: String,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): ServerOptions {
            val values = mutableMapOf<String, MutableList<String>>()
            var index = 0
            while (index < args.size) {
                val key = args[index]
                require(key.startsWith("--")) { "Unexpected argument '$key'." }
                require(index + 1 < args.size) { "Missing value for '$key'." }
                values.getOrPut(key) { mutableListOf() }.add(args[index + 1])
                index += 2
            }

            fun get(name: String): String? = values[name]?.lastOrNull()
            return ServerOptions(
                serverEndpoint = get("--server-endpoint") ?: "",
                logDir = get("--log-dir") ?: "logs",
            )
        }
    }
}
