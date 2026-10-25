package MyGame.match

import MyGame.*
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class DomainTest {
    @Test
    fun queueOwnsModesAndRoomSequence() {
        val state = MatchmakingState()
        state.enqueue(QueueRequest("p1"))
        state.enqueue(QueueRequest("p1"))
        state.enqueue(QueueRequest("p2", "ranked"))
        assertTrue(state.pair().isEmpty())
        state.enqueue(QueueRequest("p3"))
        assertEquals(listOf("p1", "p3"), state.pair().map { it.playerId })
        assertEquals("room-1", state.allocateRoom())
        state.remove(state.pair())
        assertEquals(listOf("ranked"), state.waiting.keys.toList())
        assertEquals("room-2", state.allocateRoom())
    }
}
