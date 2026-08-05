package systems.zlink.samples.kotlin.bingo.server.configuration

import java.time.Duration

object SampleTimings {
    val RequestTimeout: Duration = Duration.ofSeconds(10)
    val DrawPeriod: Duration = Duration.ofMillis(20)
}
