package MyGame.town;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.*;

class TownStateTest {
    @Test
    void partyAdmissionAndLeadershipHaveOneOwner() {
        var state = new TownState();
        state.players.addAll(List.of("a", "b", "c", "d", "e"));
        assertFalse(state.party("outsider", null, true));
        assertFalse(state.party("a", "missing", false));
        assertTrue(state.party("a", null, true));
        String partyId = state.partyFor("a").partyId;
        assertTrue(Names.validId(partyId, "party-"));
        assertFalse(state.party("a", null, true));
        assertFalse(state.party("b", "missing", false));
        assertFalse(state.party("a", partyId, false));
        for (var id : List.of("b", "c", "d")) assertTrue(state.party(id, partyId, false));
        assertFalse(state.party("e", partyId, false));
        assertSame(state.parties.get(partyId), state.partyFor("a"));
        assertEquals("a", state.partyFor("b").members.getFirst());
        var snapshot = state.view("town-1", "townA");
        state.leave("a");
        assertEquals("b", state.partyFor("b").members.getFirst());
        assertEquals(partyId, state.partyFor("b").partyId);
        assertEquals(List.of("a", "b", "c", "d"), snapshot.parties().getFirst().members());
        for (var id : List.of("b", "c", "d")) state.leave(id);
        assertTrue(state.parties.isEmpty());
    }

    @Test
    void relocationPreservesPartyRules() throws Exception {
        var state = new TownState();
        state.players.addAll(List.of("a", "b"));
        state.party("a", null, true);
        var partyId = state.partyFor("a").partyId;
        state.party("b", partyId, false);
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), TownState.class);
        assertEquals(state.view("town-1", "townA"), restored.view("town-1", "townA"));
        restored.leave("a");
        assertEquals(List.of("b"), restored.parties.get(partyId).members);
    }

}
