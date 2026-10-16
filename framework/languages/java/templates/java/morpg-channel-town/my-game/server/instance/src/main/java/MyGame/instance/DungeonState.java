package MyGame.instance;

import MyGame.*;

import java.util.*;

public final class DungeonState {
    public String partyId = "";
    public List<String> members = List.of();
    public int stage;

    public void prepare(String dungeonId, PrepareDungeon request) {
        if (!partyId.isEmpty()
                || request.partyId() == null
                || request.partyId().isBlank()
                || request.members().isEmpty()
                || request.members().size() > Rules.MAX_MEMBERS
                || new HashSet<>(request.members()).size() != request.members().size()
                || request.members().stream().anyMatch(String::isBlank)
                || !(Names.DUNGEON_PREFIX + request.partyId()).equals(dungeonId))
            throw new IllegalArgumentException("Invalid dungeon party preparation");
        partyId = request.partyId();
        members = List.copyOf(request.members());
    }
}
