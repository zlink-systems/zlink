package MyGame.region.domain

import MyGame.*
import com.fasterxml.jackson.core.type.TypeReference
import kotlin.test.*

class PositionsTest {
    @Test
    fun capacityRejectsNewPlayerAndRetainsExistingPosition() {
        val positions = Positions()
        assertTrue(positions.admit("p1", 2))
        positions.move("p1", 10.0, 1)
        assertTrue(positions.admit("p2", 2))
        assertFalse(positions.admit("p3", 2))
        assertTrue(positions.admit("p1", 2))
        assertEquals(listOf(PlayerPosition("p1", 10.0, 1), PlayerPosition("p2", 0.0, 0)), positions.view())
    }

    @Test
    fun relocationRoundTripPreservesSequenceAndIndependentState() {
        val source = Positions { 0L }
        source.admit("p1", 2)
        source.move("p1", 10.0, 1)
        val bytes = Json.mapper.writeValueAsBytes(source.snapshot())
        val target = Positions { 0L }
        target.restore(
            Json.mapper.readValue(
                bytes,
                object : TypeReference<LinkedHashMap<String, Position>>() {},
            )
        )
        assertEquals(source.view(), target.view())
        assertEquals(ActionRejected("stale-sequence", 0), target.move("p1", 0.0, 1))
        assertNull(target.move("p1", 0.0, 2))
        assertEquals(listOf(PlayerPosition("p1", 10.0, 1)), source.view())
        assertEquals(listOf(PlayerPosition("p1", 10.0, 2)), target.view())
    }

    @Test
    fun leavingOneInstanceCannotChangeAnotherInstance() {
        val first = Positions()
        val second = Positions()
        first.admit("p1", 2)
        second.admit("p3", 2)
        first.leave("p1")
        assertTrue(first.view().isEmpty())
        assertEquals(listOf(PlayerPosition("p3", 0.0, 0)), second.view())
    }
}
