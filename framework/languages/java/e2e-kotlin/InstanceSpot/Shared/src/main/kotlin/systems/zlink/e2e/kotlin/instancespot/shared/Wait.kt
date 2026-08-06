package systems.zlink.e2e.kotlin.instancespot.shared

import java.time.Duration
import java.time.Instant

object Wait {
    fun <T : Any> until(timeout: Duration, message: String, attempt: () -> T?): T {
        val deadline = Instant.now().plus(timeout)
        var last: Throwable? = null
        while (Instant.now().isBefore(deadline)) {
            try {
                attempt()?.let { return it }
            } catch (error: Throwable) {
                last = error
            }
            Thread.sleep(50)
        }
        throw IllegalStateException(message, last)
    }
}
