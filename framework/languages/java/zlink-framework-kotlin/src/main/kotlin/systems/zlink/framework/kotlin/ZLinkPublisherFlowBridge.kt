@file:JvmName("ZLinkLocationExtensionsKt")
@file:JvmMultifileClass

package systems.zlink.framework.kotlin

import java.util.concurrent.Flow.Publisher
import java.util.concurrent.Flow.Subscriber
import java.util.concurrent.Flow.Subscription
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
fun <T> Publisher<T>.asFlow(): Flow<T> = callbackFlow {
    val subscriber = object : Subscriber<T> {
        private var subscription: Subscription? = null

        override fun onSubscribe(subscription: Subscription) {
            this.subscription = subscription
            subscription.request(Long.MAX_VALUE)
        }

        override fun onNext(item: T) {
            trySend(item)
        }

        override fun onError(throwable: Throwable) {
            close(throwable)
        }

        override fun onComplete() {
            close()
        }

        fun cancel() {
            subscription?.cancel()
        }
    }
    subscribe(subscriber)
    awaitClose { subscriber.cancel() }
}
