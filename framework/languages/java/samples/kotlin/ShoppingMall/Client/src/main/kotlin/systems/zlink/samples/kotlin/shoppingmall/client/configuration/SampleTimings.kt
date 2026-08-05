package systems.zlink.samples.kotlin.shoppingmall.client.configuration

import java.time.Duration

object SampleTimings {
    val RequestTimeout: Duration = Duration.ofSeconds(10)
    val WorkflowTimeout: Duration = Duration.ofSeconds(12)
    val PollDelay: Duration = Duration.ofMillis(100)
}
