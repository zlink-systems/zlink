package MyGame.instance;

import MyGame.*;

import java.util.*;

public final class DungeonState {
    public String partyId = "";
    public String runId = "";
    public String ownerSpotId = "";
    public List<String> members = List.of();
    public Set<String> joined = new LinkedHashSet<>();
    public RunPhase phase = RunPhase.Forming;
    public long stage;

    public String prepare(String dungeonId, PrepareDungeon request) {
        if (request == null || request.members() == null) return Rules.RUN_MISMATCH;
        if (!runId.isEmpty())
            return Objects.equals(runId, request.runId())
                            && Objects.equals(partyId, request.partyId())
                            && Objects.equals(ownerSpotId, request.ownerSpotId())
                            && members.equals(request.members())
                    ? null
                    : Rules.RUN_MISMATCH;
        if (!Names.validId(request.runId(), "run-")
                || !Names.validId(request.partyId(), "party-")
                || request.ownerSpotId() == null
                || request.ownerSpotId().isBlank()
                || !Names.dungeonId(request.runId()).equals(dungeonId)
                || request.members().isEmpty()
                || request.members().size() > Rules.MAX_MEMBERS
                || new HashSet<>(request.members()).size() != request.members().size()
                || request.members().stream().anyMatch(member -> member == null || member.isBlank()))
            return Rules.RUN_MISMATCH;
        runId = request.runId();
        partyId = request.partyId();
        ownerSpotId = request.ownerSpotId();
        members = List.copyOf(request.members());
        phase = RunPhase.Entering;
        return null;
    }

    public boolean accepts(String actor, String dungeonId, JoinDungeon request) {
        return request != null
                && Objects.equals(request.dungeonId(), dungeonId)
                && Objects.equals(request.runId(), runId)
                && members.contains(actor);
    }

    public boolean join(String actor) {
        if (!members.contains(actor)) return false;
        joined.add(actor);
        if (phase != RunPhase.Entering || joined.size() != members.size()) return false;
        phase = RunPhase.Active;
        return true;
    }

    public String advance() {
        if (stage < 0 || stage >= Names.MAXIMUM_INTEGER) return Rules.INTEGER_OUT_OF_RANGE;
        if (phase != RunPhase.Active) return Rules.RUN_NOT_ACTIVE;
        stage++;
        if (stage == Names.FINAL_STAGE) phase = RunPhase.Completed;
        return null;
    }
}
