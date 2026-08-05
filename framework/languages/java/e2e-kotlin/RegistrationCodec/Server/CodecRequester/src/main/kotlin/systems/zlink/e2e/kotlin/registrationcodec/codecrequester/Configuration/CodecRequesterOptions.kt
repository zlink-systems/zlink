package systems.zlink.e2e.kotlin.registrationcodec.codecrequester.configuration

data class CodecRequesterOptions(
    val httpEndpoint: String,
    val serverEndpoint: String,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): CodecRequesterOptions {
            val values = mutableMapOf<String, MutableList<String>>()
            var index = 0
            while (index < args.size) {
                val key = args[index]
                require(key.startsWith("--")) { "Unexpected argument '$key'." }
                require(index + 1 < args.size) { "Missing value for '$key'." }
                values.getOrPut(key) { mutableListOf() }.add(args[index + 1])
                index += 2
            }

            fun required(name: String): String =
                values[name]?.lastOrNull()?.takeIf { it.isNotBlank() }
                    ?: throw IllegalArgumentException("$name is required.")
            fun get(name: String): String? = values[name]?.lastOrNull()
            return CodecRequesterOptions(
                httpEndpoint = required("--http-endpoint"),
                serverEndpoint = required("--server-endpoint"),
                logDir = get("--log-dir") ?: "logs",
            )
        }
    }
}
