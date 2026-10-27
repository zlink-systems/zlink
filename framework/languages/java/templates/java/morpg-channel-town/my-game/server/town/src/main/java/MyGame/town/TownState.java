package MyGame.town;

import MyGame.*;

import java.util.*;

public final class TownState {
    public Set<String> players = new LinkedHashSet<>();
    public Map<String, PartyRun> parties = new LinkedHashMap<>();

    public void leave(String id) {
        players.remove(id);
        var it = parties.entrySet().iterator();
        while (it.hasNext()) {
            var party = it.next().getValue();
            party.leave(id);
            if (party.members.isEmpty()) it.remove();
        }
    }

    public boolean party(String id, String partyId, boolean create) {
        if (!players.contains(id) || partyFor(id) != null) return false;
        if (create) {
            var party = new PartyRun();
            party.members.add(id);
            parties.put(party.partyId, party);
            return true;
        }
        if (partyId == null || partyId.isBlank()) return false;
        var party = parties.get(partyId);
        if (party == null
                || party.phase != RunPhase.Forming
                || party.members.size() >= Rules.MAX_MEMBERS) return false;
        party.members.add(id);
        return true;
    }

    public PartyRun partyFor(String id) {
        return parties.values().stream().filter(party -> party.members.contains(id)).findFirst().orElse(null);
    }

    public PartyRun partyForRun(String runId) {
        return parties.values().stream()
                .filter(party -> Objects.equals(party.runId, runId))
                .findFirst()
                .orElse(null);
    }

    public TownView view(String townId, String nodeId) {
        return new TownView(
                townId,
                nodeId,
                players.stream().sorted().toList(),
                new TreeMap<>(parties)
                        .entrySet().stream()
                                .map(e -> e.getValue().view())
                                .toList());
    }
}
