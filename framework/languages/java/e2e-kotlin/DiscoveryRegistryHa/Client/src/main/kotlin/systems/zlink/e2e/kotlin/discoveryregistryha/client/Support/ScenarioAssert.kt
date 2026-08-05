package systems.zlink.e2e.kotlin.discoveryregistryha.client.Support

object ScenarioAssert {
    fun that(condition: Boolean, message: String) {
        if (!condition) {
            throw IllegalStateException(message)
        }
    }
}
