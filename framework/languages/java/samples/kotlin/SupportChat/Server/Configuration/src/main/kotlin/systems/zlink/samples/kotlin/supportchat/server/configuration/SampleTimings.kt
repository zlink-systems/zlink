package systems.zlink.samples.kotlin.supportchat.server.configuration

import java.time.Duration

object SampleTimings {
    val ConnectTimeout: Duration = Duration.ofSeconds(5)
    val RequestTimeout: Duration = Duration.ofSeconds(5)
    val IdleTimeout: Duration = Duration.ofSeconds(3)
    val CloseGraceTimeout: Duration = Duration.ofSeconds(2)
}
