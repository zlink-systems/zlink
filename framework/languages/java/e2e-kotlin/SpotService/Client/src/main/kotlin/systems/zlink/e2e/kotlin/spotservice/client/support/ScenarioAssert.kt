package systems.zlink.e2e.kotlin.spotservice.client.support

internal fun ensure(condition: Boolean, message: String) {
    if (!condition) {
        throw IllegalStateException(message)
    }
}

internal suspend fun expectFailure(action: suspend () -> Unit) {
    try {
        action()
    } catch (_: Exception) {
        return
    }
    throw IllegalStateException("operation unexpectedly succeeded")
}
