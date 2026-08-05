package systems.zlink.e2e.kotlin.spotservice.client.support

import java.time.Duration
import kotlinx.coroutines.delay

private val EVENTUAL_TIMEOUT: Duration = Duration.ofSeconds(30)

internal suspend fun <T> eventually(action: suspend () -> T): T {
    val deadline = System.nanoTime() + EVENTUAL_TIMEOUT.toNanos()
    var lastFailure: Exception? = null
    while (System.nanoTime() < deadline) {
        try {
            return action()
        } catch (error: Exception) {
            lastFailure = error
            sleep(200)
        }
    }
    throw IllegalStateException("operation did not succeed before timeout", lastFailure)
}

internal suspend fun sleep(millis: Long) = delay(millis)
