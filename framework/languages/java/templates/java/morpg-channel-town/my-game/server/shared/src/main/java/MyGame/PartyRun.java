package MyGame;

import java.util.*;

public final class PartyRun {
    public String partyId = "party-" + UUID.randomUUID().toString().replace("-", "");
    public List<String> members = new ArrayList<>();
    public RunPhase phase = RunPhase.Forming;
    public String runId;

    public String enter(String player) {
        if (members.isEmpty() || !members.getFirst().equals(player)) return Rules.NOT_LEADER;
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = "run-" + UUID.randomUUID().toString().replace("-", "");
            phase = RunPhase.Preparing;
        }
        return null;
    }

    public String prepared(String id) {
        if (!Objects.equals(runId, id)
                || (phase != RunPhase.Preparing && phase != RunPhase.Entering))
            return Rules.RUN_MISMATCH;
        phase = RunPhase.Entering;
        return null;
    }

    public String started(String id) {
        if (!Objects.equals(runId, id)
                || (phase != RunPhase.Entering && phase != RunPhase.Active))
            return Rules.RUN_MISMATCH;
        phase = RunPhase.Active;
        return null;
    }

    public String completed(String id) {
        if (!Objects.equals(runId, id)
                || (phase != RunPhase.Active && phase != RunPhase.Completed))
            return Rules.RUN_MISMATCH;
        phase = RunPhase.Completed;
        return null;
    }

    public void leave(String player) {
        if (phase == RunPhase.Forming) members.remove(player);
    }

    public String changeChannel() {
        return phase == RunPhase.Forming ? null : Rules.RUN_IN_PROGRESS;
    }

    public PartyView view() {
        return new PartyView(partyId, List.copyOf(members), phase, runId);
    }
}
