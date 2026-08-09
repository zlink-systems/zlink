package systems.zlink.e2e.kotlin.pubsub.subscriber

import java.util.concurrent.Flow
import java.util.concurrent.atomic.AtomicBoolean
import org.springframework.beans.factory.ObjectProvider
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.framework.monitoring.ZLinkFanoutStatus
import systems.zlink.framework.monitoring.ZLinkObservedStatus
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime

data class FanoutObserverEntry(
    val observer: String,
    val sequence: Long,
    val state: String,
    val readyPublisherCount: Int,
    val lossCoalesced: Long,
    val lossDiscardedTerminal: Long,
)

class FanoutObserverController(
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
) {
    private val observers = linkedMapOf<String, ControlledObserver>()
    private val monitor = Object()
    private val entries = mutableListOf<FanoutObserverEntry>()

    fun start(name: String, capacity: Int, slow: Boolean) {
        require(name.isNotBlank()) { "observer name is required" }
        require(capacity > 0) { "observer capacity must be positive" }
        cancel(name)
        val observer = ControlledObserver(name, slow)
        synchronized(monitor) {
            observers[name] = observer
        }
        runtime.getObject().fanoutRuntime().observe(Contracts.EVENT_CHANNEL, capacity).subscribe(observer)
    }

    fun release(name: String) {
        synchronized(monitor) {
            observers[name]?.release()
        }
    }

    fun cancel(name: String) {
        synchronized(monitor) {
            observers.remove(name)?.cancel()
        }
    }

    fun waitFor(name: String, timeoutMillis: Long) {
        val deadline = System.nanoTime() + timeoutMillis * 1_000_000L
        synchronized(monitor) {
            while (entries.none { it.observer == name } && System.nanoTime() < deadline) {
                monitor.wait(((deadline - System.nanoTime()) / 1_000_000L).coerceAtLeast(1L).coerceAtMost(250L))
            }
            check(entries.any { it.observer == name }) { "observer $name did not receive a status" }
        }
    }

    fun snapshot(): List<FanoutObserverEntry> = synchronized(monitor) { entries.toList() }

    private inner class ControlledObserver(
        private val name: String,
        private val slow: Boolean,
    ) : Flow.Subscriber<ZLinkObservedStatus<ZLinkFanoutStatus>> {
        private var subscription: Flow.Subscription? = null
        private val released = AtomicBoolean(!slow)

        override fun onSubscribe(value: Flow.Subscription) {
            subscription = value
            value.request(1)
        }

        override fun onNext(value: ZLinkObservedStatus<ZLinkFanoutStatus>) {
            val status = value.status()
            synchronized(monitor) {
                entries += FanoutObserverEntry(
                    name,
                    status.sequence(),
                    status.state().name,
                    status.readyPublisherCount(),
                    value.loss().coalescedCount(),
                    value.loss().discardedTerminalCount(),
                )
                monitor.notifyAll()
                while (!released.get()) {
                    try {
                        monitor.wait(250L)
                    } catch (error: InterruptedException) {
                        Thread.currentThread().interrupt()
                        return
                    }
                }
            }
            subscription?.request(1)
        }

        override fun onError(error: Throwable) {
            synchronized(monitor) {
                monitor.notifyAll()
            }
        }

        override fun onComplete() {
            synchronized(monitor) {
                monitor.notifyAll()
            }
        }

        fun release() {
            released.set(true)
            synchronized(monitor) {
                monitor.notifyAll()
            }
        }

        fun cancel() {
            released.set(true)
            subscription?.cancel()
            synchronized(monitor) {
                monitor.notifyAll()
            }
        }
    }
}
