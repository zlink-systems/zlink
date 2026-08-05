package systems.zlink.e2e.kotlin.resiliencelifecycle

data class ClientOptions(
    val mode: String,
    val consumerHttpEndpoint: String,
    val logDir: String,
    val httpAEndpoint: String?,
    val httpBEndpoint: String?,
    val apiBEndpoint: String?,
    val apiAReplacementEndpoint: String?,
    val apiBGreenEndpoint: String?,
    val httpBGreenEndpoint: String?,
    val controlDir: String?,
    val scenario: String,
    val stormExitDelayMillis: Long,
) {
    companion object {
        fun fromEnv(): ClientOptions =
            ClientOptions(
                mode = Env.get("e2e.client.mode", "default"),
                consumerHttpEndpoint = Env.get("e2e.consumer.http.endpoint"),
                logDir = Env.get("e2e.log.dir", "logs"),
                httpAEndpoint = optional("e2e.http.a.endpoint"),
                httpBEndpoint = optional("e2e.http.b.endpoint"),
                apiBEndpoint = optional("e2e.api.b.endpoint"),
                apiAReplacementEndpoint = optional("e2e.api.a.replacement.endpoint"),
                apiBGreenEndpoint = optional("e2e.api.b.green.endpoint"),
                httpBGreenEndpoint = optional("e2e.http.b.green.endpoint"),
                controlDir = optional("e2e.control.dir"),
                scenario = Env.get("e2e.scenario", "all"),
                stormExitDelayMillis = Env.get("e2e.storm.exit.delay.ms", "0").toLong(),
            )

        private fun optional(name: String): String? =
            Env.get(name, "").takeIf { it.isNotBlank() }
    }
}
