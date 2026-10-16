package MyGame.e2e

inline fun <T> withCleanup(cleanup: () -> Unit, action: () -> T): T {
    var primary: Throwable? = null
    try {
        return action()
    } catch (failure: Throwable) {
        primary = failure
        throw failure
    } finally {
        try {
            cleanup()
        } catch (failure: Throwable) {
            if (primary == null) throw failure
            if (primary !== failure) primary.addSuppressed(failure)
        }
    }
}
