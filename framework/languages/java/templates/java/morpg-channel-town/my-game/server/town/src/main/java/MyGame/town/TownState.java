package MyGame.town;

import MyGame.*;

import java.util.*;

public final class TownState {
    public Set<String> players = new LinkedHashSet<>();
    public Map<String, List<String>> parties = new LinkedHashMap<>();

    public void leave(String id) {
        players.remove(id);
        var it = parties.values().iterator();
        while (it.hasNext()) {
            var members = it.next();
            members.remove(id);
            if (members.isEmpty()) it.remove();
        }
    }

    public boolean party(String id, String partyId, boolean create) {
        if (partyId == null
                || partyId.isBlank()
                || !players.contains(id)
                || parties.values().stream().anyMatch(m -> m.contains(id))) return false;
        var members = parties.get(partyId);
        if (create ? members != null : members == null || members.size() >= Rules.MAX_MEMBERS)
            return false;
        if (create) {
            members = new ArrayList<>();
            parties.put(partyId, members);
        }
        members.add(id);
        return true;
    }

    public Map.Entry<String, List<String>> ledParty(String id) {
        return parties.entrySet().stream()
                .filter(e -> e.getValue().getFirst().equals(id))
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
                                .map(e -> new PartyView(e.getKey(), List.copyOf(e.getValue())))
                                .toList());
    }
}
