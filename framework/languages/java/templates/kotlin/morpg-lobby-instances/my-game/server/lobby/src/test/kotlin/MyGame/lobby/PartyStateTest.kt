package MyGame.lobby

import MyGame.*
import MyGame.lobby.domain.PartyState
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class PartyStateTest {
    @Test
    fun admissionEnforcesIdentityAndCapacity() {
        val state = PartyState()
        assertFalse(state.join("p1", PartyJoin("", true)))
        assertFalse(state.join("p1", PartyJoin("alpha", false)))
        assertTrue(state.join("p1", PartyJoin("alpha", true)))
        assertFalse(state.join("p1", PartyJoin("alpha", false)))
        assertFalse(state.join("p2", PartyJoin("beta", false)))
        assertFalse(state.join("p2", PartyJoin("alpha", true)))
        for (n in 2..Rules.MAX_MEMBERS) assertTrue(state.join("p$n", PartyJoin("alpha", false)))
        assertFalse(state.join("overflow", PartyJoin("alpha", false)))
    }

    @Test
    fun entryRequiresLeaderAndEveryMemberReady() {
        val state = PartyState()
        state.join("p1", PartyJoin("alpha", true))
        state.join("p2", PartyJoin("alpha", false))
        assertEquals(Rules.NOT_LEADER, state.entryRejection("p2"))
        assertEquals(Rules.NOT_READY, state.entryRejection("p1"))
        state.ready.add("p1")
        assertEquals(Rules.NOT_READY, state.entryRejection("p1"))
        state.ready.add("p2")
        assertNull(state.entryRejection("p1"))
        assertEquals(listOf(true, true), state.view().ready)
    }

    @Test
    fun transferPreservesOrderedRosterAndReadiness() {
        val state = PartyState()
        state.join("p1", PartyJoin("alpha", true))
        state.join("p2", PartyJoin("alpha", false))
        state.ready.add("p2")
        val restored =
            Json.mapper.readValue(Json.mapper.writeValueAsBytes(state), PartyState::class.java)
        assertEquals(state.view(), restored.view())
        restored.leave("p1")
        assertEquals(listOf("p2"), restored.view().members)
        assertNull(restored.entryRejection("p2"))
        assertEquals(listOf("p1", "p2"), state.view().members)
    }
}
