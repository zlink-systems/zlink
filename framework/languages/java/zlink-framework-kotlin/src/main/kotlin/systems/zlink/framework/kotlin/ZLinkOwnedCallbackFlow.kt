package systems.zlink.framework.kotlin

import java.util.concurrent.CancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * Bridges one connector callback registration to a Flow without dropping items. Items the collector
 * has not taken when collection ends are passed to [release].
 */
@PublishedApi
internal fun <T> ownedCallbackFlow(
    register: (accept: (T) -> Unit) -> AutoCloseable,
    release: (T) -> Unit,
): Flow<T> = flow {
    val items = Channel<T>(Channel.UNLIMITED, onUndeliveredElement = release)
    val registration = register { item -> if (items.trySend(item).isFailure) release(item) }
    try {
        for (item in items) {
            try {
                emit(item)
            } catch (cancelled: CancellationException) {
                release(item)
                throw cancelled
            }
        }
    } finally {
        try {
            registration.close()
        } finally {
            items.cancel()
        }
    }
}
