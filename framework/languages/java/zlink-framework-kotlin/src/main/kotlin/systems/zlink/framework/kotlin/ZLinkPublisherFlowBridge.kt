@file:JvmName("ZLinkLocationExtensionsKt")
@file:JvmMultifileClass

package systems.zlink.framework.kotlin

import java.util.concurrent.Flow.Publisher
import java.util.concurrent.Flow.Subscriber
import java.util.concurrent.Flow.Subscription
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.buffer
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch

fun <T> Publisher<T>.asFlow(): Flow<T> = callbackFlow {
    val subscriptionRef = AtomicReference<Subscription?>()
    val pending = Channel<T>(capacity = 1)
    val forward = launch {
        try {
            for (item in pending) {
                // Wait for the rendezvous collector before requesting the
                // next item so publisher loss accounting remains observable.
                send(item)
                subscriptionRef.get()?.request(1)
            }
            close()
        } catch (cancelled: kotlinx.coroutines.CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            close(failure)
        }
    }
    val subscriber = object : Subscriber<T> {

        override fun onSubscribe(subscription: Subscription) {
            if (!subscriptionRef.compareAndSet(null, subscription)) {
                subscription.cancel()
                return
            }
            subscription.request(1)
        }

        override fun onNext(item: T) {
            if (pending.trySend(item).isFailure) {
                subscriptionRef.get()?.cancel()
                close(IllegalStateException(
                    "publisher delivered more than its requested demand"))
            }
        }

        override fun onError(throwable: Throwable) {
            pending.close(throwable)
            close(throwable)
        }

        override fun onComplete() {
            pending.close()
        }
    }
    subscribe(subscriber)
    awaitClose {
        subscriptionRef.get()?.cancel()
        pending.close()
        forward.cancel()
    }
}.buffer(capacity = 0)
