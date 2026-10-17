package MyGame.lobby;

import MyGame.*;

import java.util.*;

public final class PartyState {
    public String partyId = "";
    public List<String> members = new ArrayList<>();
    public Set<String> ready = new LinkedHashSet<>();

    public boolean join(String id, PartyJoin request) {
        if (request.partyId() == null
                || request.partyId().isBlank()
                || members.contains(id)
                || members.size() >= Rules.MAX_MEMBERS
                || (request.create()
                        ? !members.isEmpty()
                        : !partyId.equals(request.partyId()) || members.isEmpty())) return false;
        partyId = request.partyId();
        members.add(id);
        return true;
    }

    public void leave(String id) {
        members.remove(id);
        ready.remove(id);
    }

    public PartyView view() {
        return new PartyView(
                partyId, List.copyOf(members), members.stream().map(ready::contains).toList());
    }

    public String entryRejection(String id) {
        return members.isEmpty() || !members.getFirst().equals(id)
                ? Rules.NOT_LEADER
                : members.stream().anyMatch(m -> !ready.contains(m)) ? Rules.NOT_READY : null;
    }
}
