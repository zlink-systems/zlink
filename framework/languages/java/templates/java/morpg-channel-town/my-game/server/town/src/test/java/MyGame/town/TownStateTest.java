package MyGame.town;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;
import java.util.*;
import org.junit.jupiter.api.Test;

class TownStateTest {
  @Test
  void partyAdmissionAndLeadershipHaveOneOwner() {
    var state = new TownState();
    state.players.addAll(List.of("a", "b", "c", "d", "e"));
    assertFalse(state.party("outsider", "party", true));
    assertFalse(state.party("a", "", true));
    assertTrue(state.party("a", "party", true));
    assertFalse(state.party("a", "other", true));
    assertFalse(state.party("b", "party", true));
    assertFalse(state.party("b", "missing", false));
    for (var id : List.of("b", "c", "d")) assertTrue(state.party(id, "party", false));
    assertFalse(state.party("e", "party", false));
    assertEquals("party", state.ledParty("a").getKey());
    assertNull(state.ledParty("b"));
    var snapshot = state.view("town-1", "townA");
    state.leave("a");
    assertEquals("party", state.ledParty("b").getKey());
    assertEquals(List.of("a", "b", "c", "d"), snapshot.parties().getFirst().members());
    for (var id : List.of("b", "c", "d")) state.leave(id);
    assertTrue(state.parties.isEmpty());
  }

  @Test
  void relocationPreservesPartyRules() throws Exception {
    var state = new TownState();
    state.players.addAll(List.of("a", "b"));
    state.party("a", "party", true);
    state.party("b", "party", false);
    var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), TownState.class);
    assertEquals(state.view("town-1", "townA"), restored.view("town-1", "townA"));
    restored.leave("a");
    assertEquals(List.of("b"), restored.parties.get("party"));
  }
}
