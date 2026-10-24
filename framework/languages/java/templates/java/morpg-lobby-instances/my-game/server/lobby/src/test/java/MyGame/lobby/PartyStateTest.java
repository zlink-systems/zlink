package MyGame.lobby;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.List;

class PartyStateTest {
    @Test
    void admissionEnforcesPartyIdentityAndCapacity() {
        var s = new PartyState();
        assertFalse(s.join("p1", new PartyJoin("", true)));
        assertFalse(s.join("p1", new PartyJoin("alpha", false)));
        assertTrue(s.join("p1", new PartyJoin("alpha", true)));
        assertFalse(s.join("p1", new PartyJoin("alpha", false)));
        assertFalse(s.join("p2", new PartyJoin("beta", false)));
        assertFalse(s.join("p2", new PartyJoin("alpha", true)));
        for (int i = 2; i <= Rules.MAX_MEMBERS; i++)
            assertTrue(s.join("p" + i, new PartyJoin("alpha", false)));
        assertFalse(s.join("overflow", new PartyJoin("alpha", false)));
    }

    @Test
    void entryRequiresLeaderAndEveryMemberReady() {
        var s = new PartyState();
        s.join("p1", new PartyJoin("alpha", true));
        s.join("p2", new PartyJoin("alpha", false));
        assertEquals(Rules.NOT_LEADER, s.entryRejection("p2"));
        assertEquals(Rules.NOT_READY, s.entryRejection("p1"));
        s.ready.add("p1");
        assertEquals(Rules.NOT_READY, s.entryRejection("p1"));
        s.ready.add("p2");
        assertNull(s.entryRejection("p1"));
        assertEquals(List.of(true, true), s.view().ready());
    }

    @Test
    void relocationPreservesOrderedRosterAndReadiness() throws Exception {
        var s = new PartyState();
        s.join("p1", new PartyJoin("alpha", true));
        s.join("p2", new PartyJoin("alpha", false));
        s.ready.add("p2");
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(s), PartyState.class);
        assertEquals(s.view(), restored.view());
        restored.leave("p1");
        assertEquals(List.of("p2"), restored.view().members());
        assertNull(restored.entryRejection("p2"));
        assertEquals(List.of("p1", "p2"), s.view().members());
    }
}
