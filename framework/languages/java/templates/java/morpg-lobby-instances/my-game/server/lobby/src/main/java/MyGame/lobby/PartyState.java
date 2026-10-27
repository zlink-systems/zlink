package MyGame.lobby;

import MyGame.*;

import java.util.*;

public final class PartyState {
    public String partyId = "";
    public RunPhase phase = RunPhase.Forming;
    public String runId;
    public Set<String> entered = new LinkedHashSet<>();
    public List<String> members = new ArrayList<>();
    public Set<String> ready = new LinkedHashSet<>();

    public boolean join(String id, PartyJoin request) {
        if (phase != RunPhase.Forming || request.partyId() == null
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
        if (phase != RunPhase.Forming) return;
        members.remove(id);
        ready.remove(id);
    }

    public PartyView view() {
        return new PartyView(
                partyId, List.copyOf(members), members.stream().map(ready::contains).toList(), phase, runId);
    }

    public String entryRejection(String id) {
        return members.isEmpty() || !members.getFirst().equals(id)
                ? Rules.NOT_LEADER
                : members.stream().anyMatch(m -> !ready.contains(m)) ? Rules.NOT_READY : null;
    }
    public void markReady(String id) { if (phase == RunPhase.Forming && members.contains(id)) ready.add(id); }
    public String enter(String id) {
        String reason = entryRejection(id); if (reason != null) return reason;
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = Names.newRunId(); phase = RunPhase.Preparing; entered.clear();
        }
        return null;
    }
    public String arrived(String run, String player) {
        if (!Objects.equals(runId, run) || phase == RunPhase.Forming || !members.contains(player)) return Rules.RUN_MISMATCH;
        entered.add(player); return null;
    }
    public String prepared(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Preparing && phase != RunPhase.Entering)) return Rules.RUN_MISMATCH;
        phase = RunPhase.Entering; return null;
    }
    public String started(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Entering && phase != RunPhase.Active)) return Rules.RUN_MISMATCH;
        phase = RunPhase.Active; return null;
    }
    public String completed(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Active && phase != RunPhase.Completed)) return Rules.RUN_MISMATCH;
        phase = RunPhase.Completed; return null;
    }
}
