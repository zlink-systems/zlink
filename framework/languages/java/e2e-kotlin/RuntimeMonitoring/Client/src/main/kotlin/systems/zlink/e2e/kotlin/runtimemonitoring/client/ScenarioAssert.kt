package systems.zlink.e2e.kotlin.runtimemonitoring.client

object ScenarioAssert {
    fun ensure(condition: Boolean, message: String) {
        if (!condition) {
            throw IllegalStateException(message)
        }
    }

    fun sleep(millis: Long) {
        try {
            Thread.sleep(millis)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("interrupted", error)
        }
    }
}
