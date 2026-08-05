package systems.zlink.e2e.kotlin.registrationcodec.client.support

import java.time.Duration

class ScenarioAssert {
    fun that(condition: Boolean, message: String) {
        if (!condition) {
            throw IllegalStateException(message)
        }
    }

    fun waitUntil(name: String, condition: () -> Boolean) {
        val deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos()
        while (System.nanoTime() < deadline) {
            if (condition()) {
                return
            }
            Thread.sleep(100)
        }
        throw IllegalStateException("timed out waiting for $name")
    }
}
