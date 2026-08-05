package systems.zlink.framework.kotlin

import java.util.concurrent.CompletionStage
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

public suspend fun <T> CompletionStage<T>.await(): T =
    awaitFrameworkStage(this)

@PublishedApi
internal suspend fun <T> awaitFrameworkStage(stage: CompletionStage<T>): T {
    return suspendCancellableCoroutine { continuation ->
        continuation.invokeOnCancellation {
            stage.toCompletableFuture().cancel(false)
        }
        stage.whenComplete { value, error ->
            if (error == null) {
                continuation.resume(value)
            } else {
                continuation.resumeWithException(unwrapCompletionError(error))
            }
        }
    }
}

private fun unwrapCompletionError(error: Throwable): Throwable =
    when (error) {
        is java.util.concurrent.CompletionException,
        is java.util.concurrent.ExecutionException,
        -> error.cause ?: error
        else -> error
    }
