package MyGame.instance;
import MyGame.*;
import java.util.*;
public final class DungeonState {
    public String partyId = "", runId;
    public List<String> members = List.of();
    public Set<String> joined = new LinkedHashSet<>();
    public long stage;
    public RunPhase phase = RunPhase.Preparing;
    public String prepare(PrepareDungeon r) {
        if (runId != null) return runId.equals(r.runId()) && partyId.equals(r.partyId()) && members.equals(r.members()) ? null : Rules.RUN_MISMATCH;
        if (r.runId() == null || r.runId().isBlank() || r.partyId() == null || r.partyId().isBlank() || r.members().isEmpty() || r.members().size() > Rules.MAX_MEMBERS || new HashSet<>(r.members()).size() != r.members().size() || r.members().stream().anyMatch(String::isBlank)) return Rules.RUN_MISMATCH;
        runId = r.runId(); partyId = r.partyId(); members = List.copyOf(r.members()); phase = RunPhase.Entering; return null;
    }
    public boolean admit(String id, JoinDungeon r, String dungeonId) { return r.dungeonId().equals(dungeonId) && Objects.equals(r.runId(), runId) && members.contains(id); }
    public boolean join(String id) {
        if (!members.contains(id)) return false;
        joined.add(id); if (phase != RunPhase.Entering || joined.size() != members.size()) return false;
        phase = RunPhase.Active; return true;
    }
    public String advance() {
        if (stage >= Rules.MAXIMUM_INTEGER || stage < 0) return Rules.INTEGER_OUT_OF_RANGE;
        if (phase != RunPhase.Active) return Rules.RUN_NOT_ACTIVE;
        stage++; if (stage == Rules.FINAL_STAGE) phase = RunPhase.Completed; return null;
    }
}
