package MyGame.instance;

import MyGame.*;

import java.util.*;

public final class DungeonRun {
    public String partyId = "";
    public String runId = "";
    public List<String> members = new ArrayList<>();
    public Set<String> entered = new LinkedHashSet<>();
    public RunPhase phase = RunPhase.Preparing;
    public int stage;

    public ActionRejected prepare(PrepareDungeon request) {
        if (!runId.isEmpty())
            return runId.equals(request.runId()) && partyId.equals(request.partyId())
                    && members.equals(request.members())
                    ? null
                    : new ActionRejected(Rejection.RUN_MISMATCH);
        var requestedMembers = request.members();
        if (requestedMembers == null
                || requestedMembers.isEmpty()
                || requestedMembers.size() > Rules.MAXIMUM_PARTY_MEMBERS
                || new HashSet<>(requestedMembers).size() != requestedMembers.size()
                || requestedMembers.stream().anyMatch(id -> id == null || id.isBlank()))
            return new ActionRejected(Rejection.INVALID_PARTY);
        runId = request.runId();
        partyId = request.partyId();
        members = List.copyOf(requestedMembers);
        phase = RunPhase.Entering;
        return null;
    }

    public boolean canJoin(String actorId, String requestedRunId) {
        return runId.equals(requestedRunId) && members.contains(actorId);
    }

    public void arrived(String actorId) {
        entered.add(actorId);
        if (entered.size() == members.size() && phase == RunPhase.Entering)
            phase = RunPhase.Active;
    }

    public ActionRejected advance() {
        if (phase != RunPhase.Active) return new ActionRejected(Rejection.RUN_NOT_ACTIVE);
        stage = Math.incrementExact(stage);
        if (stage == Rules.FINAL_STAGE) phase = RunPhase.Completed;
        return null;
    }
}
