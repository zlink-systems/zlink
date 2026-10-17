package systems.zlink.framework.runtime.spots

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.future.await
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.params.ParameterizedTest
import org.junit.jupiter.params.provider.ValueSource
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext

class KotlinCanonicalJoinLifecycleTest {
    @ParameterizedTest
    @ValueSource(booleans = [false, true])
    fun localAndCanonicalSuspendCallbacksShareTheLifecycleFifo(canonicalFirst: Boolean) {
        ZLinkActorJoinLifecycleAdmissionTest.Fixture(Room::class.java).use { fixture ->
            val room = fixture.room
            try {
                val first = if (canonicalFirst) fixture.canonical(1) else fixture.local()
                room.entered.get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                val second = if (canonicalFirst) fixture.local() else fixture.canonical(2)
                fixture.drainInfrastructure()
                assertEquals(1, room.calls.size, "second callback entered a held lifecycle turn")
                assertFalse(first.toCompletableFuture().isDone)
                assertFalse(second.toCompletableFuture().isDone)
                room.release.complete(null)
                assertTrue(
                    first.toCompletableFuture().get(COMPLETION_SECONDS, TimeUnit.SECONDS).accepted()
                )
                assertTrue(
                    second
                        .toCompletableFuture()
                        .get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                        .accepted()
                )
                assertEquals(
                    if (canonicalFirst) listOf("remote-1", "local")
                    else listOf("local", "remote-2"),
                    room.calls,
                )
                assertEquals(
                    listOf(true, true),
                    room.entryTurns,
                    "callback entered outside the lifecycle FIFO",
                )
                assertEquals(
                    listOf(true, true),
                    room.resumedTurns,
                    "coroutine lost its lifecycle turn",
                )
            } finally {
                room.release.complete(null)
            }
        }
    }

    class Room(override val context: ZLinkSpotContext) : ZLinkSuspendingSpot<ZLinkActor>() {
        val calls = CopyOnWriteArrayList<String>()
        val entryTurns = CopyOnWriteArrayList<Boolean>()
        val resumedTurns = CopyOnWriteArrayList<Boolean>()
        val entered = CompletableFuture<Void>()
        val release = CompletableFuture<Void>()

        override suspend fun onActorJoinSuspending(
            actorId: String,
            request: ZLinkMessage,
        ): ZLinkSpotActorJoinResult {
            entryTurns.add(ZLinkSuspendInvocationContext.currentSerialExecutionTurn() != null)
            calls.add(actorId)
            entered.complete(null)
            if (calls.size == 1) release.await()
            resumedTurns.add(ZLinkSuspendInvocationContext.currentSerialExecutionTurn() != null)
            return ZLinkSpotActorJoinResult.accept()
        }

        override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {}

        override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {}
    }

    private companion object {
        const val COMPLETION_SECONDS = 5L
    }
}
