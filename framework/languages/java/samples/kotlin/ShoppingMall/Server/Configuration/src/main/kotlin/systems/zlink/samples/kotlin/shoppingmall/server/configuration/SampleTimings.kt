package systems.zlink.samples.kotlin.shoppingmall.server.configuration

import java.time.Duration

object SampleTimings {
    val FrameworkTimeout: Duration = Duration.ofSeconds(8)
    val RequestTimeout: Duration = Duration.ofSeconds(10)
    val WorkflowTimeout: Duration = Duration.ofSeconds(8)
    val ChannelRetryDelay: Duration = Duration.ofMillis(100)
    val PollDelay: Duration = Duration.ofMillis(100)
    const val MaxChannelAttempts = 80
}
