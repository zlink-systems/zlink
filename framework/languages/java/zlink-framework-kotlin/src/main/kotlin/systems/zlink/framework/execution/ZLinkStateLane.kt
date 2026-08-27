package systems.zlink.framework.execution

import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.coroutines.AbstractCoroutineContextElement
import kotlin.coroutines.CoroutineContext

/**
 * Single-owner execution lane for one component's mutable state.
 *
 * State owned by this lane stays in ordinary, unsynchronized collections. The lane, rather than a
 * collection-level lock, makes each state turn exclusive. This is deliberately separate from
 * [ZLinkAsyncSerialQueue], which owns Spot and Actor execution concerns such as relocation and
 * lifecycle admission.
 */
internal class ZLinkStateLane {
    private val mailbox = ConcurrentLinkedQueue<WorkItem>()
    private val scheduled = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private val drained = CompletableDeferred<Unit>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /** Returns whether this coroutine is currently executing a turn on this lane. */
    internal suspend fun isOnLane(): Boolean =
        currentCoroutineContext()[LaneContext]?.lane === this

    /**
     * Runs [work] on this lane and returns its result.
     *
     * Calling this from an active turn on the same lane fails immediately instead of waiting for
     * work which is necessarily queued behind the current turn.
     */
    internal suspend fun <T> run(work: suspend () -> T): T {
        requireNotReentrant()
        check(!closed.get()) { "The state lane is closed." }

        val completion = CompletableDeferred<T>()
        mailbox.add(
            WorkItem {
                try {
                    completion.complete(work())
                } catch (error: Throwable) {
                    completion.completeExceptionally(error)
                }
            },
        )
        scheduleDrain()
        return completion.await()
    }

    /** Queues [work] without waiting for its result. */
    internal fun tryPost(work: suspend () -> Unit): Boolean {
        if (closed.get()) {
            return false
        }

        mailbox.add(WorkItem(work))
        scheduleDrain()
        return true
    }

    /** Fails at the reentrant call site so a same-lane wait cannot become a deadlock. */
    internal suspend fun requireNotReentrant() {
        check(!isOnLane()) {
            "This code already runs on the state lane it is trying to enter. " +
                "Call the component's private state method directly instead of re-entering its public surface."
        }
    }

    private fun scheduleDrain() {
        if (scheduled.compareAndSet(false, true)) {
            scope.launch {
                drain()
            }
        }
    }

    private suspend fun drain() {
        try {
            withContext(LaneContext(this)) {
                var processed = 0
                while (processed < DRAIN_BATCH_LIMIT) {
                    val work = mailbox.poll() ?: break
                    try {
                        work.execute()
                    } catch (_: Throwable) {
                        // Posted work owns its failures. Keep draining so later state turns run.
                    }
                    processed++
                }
            }
        } finally {
            scheduled.set(false)
            if (mailbox.isNotEmpty()) {
                scheduleDrain()
            } else if (closed.get()) {
                drained.complete(Unit)
                scope.cancel()
            }
        }
    }

    /** Closes the lane after all already accepted work has run. */
    internal suspend fun closeAndJoin() {
        if (closed.compareAndSet(false, true)) {
            if (!scheduled.get() && mailbox.isEmpty()) {
                drained.complete(Unit)
                scope.cancel()
            } else {
                scheduleDrain()
            }
        }
        drained.await()
    }

    private class WorkItem(
        val execute: suspend () -> Unit,
    )

    private class LaneContext(
        val lane: ZLinkStateLane,
    ) : AbstractCoroutineContextElement(Key) {
        companion object Key : CoroutineContext.Key<LaneContext>
    }

    private companion object {
        const val DRAIN_BATCH_LIMIT = 100
    }
}
