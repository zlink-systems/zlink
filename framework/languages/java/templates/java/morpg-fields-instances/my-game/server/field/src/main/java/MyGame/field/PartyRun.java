package MyGame.field;

import MyGame.*;

import java.util.*;
import java.util.regex.Pattern;

public final class PartyRun {
    private static final Pattern LOWER_HEX_ID = Pattern.compile("^[0-9a-f]{32}$");

    public List<String> members = new ArrayList<>();
    public Set<String> entered = new LinkedHashSet<>();
    public RunPhase phase = RunPhase.Forming;
    public String runId;

    public static boolean validId(String id, String prefix) {
        return id != null && id.startsWith(prefix)
                && LOWER_HEX_ID.matcher(id.substring(prefix.length())).matches();
    }

    public ActionRejected join(String id) {
        if (phase != RunPhase.Forming || members.contains(id)
                || members.size() >= Rules.MAXIMUM_PARTY_MEMBERS || id == null || id.isBlank())
            return new ActionRejected(Rejection.INVALID_PARTY);
        members.add(id);
        return null;
    }

    public void leave(String id) {
        if (phase == RunPhase.Forming) members.remove(id);
    }

    public ActionRejected enter(String id) {
        if (members.isEmpty() || !members.getFirst().equals(id))
            return new ActionRejected(Rejection.NOT_LEADER);
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = "run-" + UUID.randomUUID().toString().replace("-", "");
            entered.clear();
            phase = RunPhase.Preparing;
        }
        return null;
    }

    public ActionRejected prepared(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Preparing && phase != RunPhase.Entering))
            return new ActionRejected(Rejection.RUN_MISMATCH);
        phase = RunPhase.Entering;
        return null;
    }

    public ActionRejected arrived(String id, String actorId) {
        if (!Objects.equals(runId, id) || !members.contains(actorId)
                || (phase != RunPhase.Entering && phase != RunPhase.Active))
            return new ActionRejected(Rejection.RUN_MISMATCH);
        entered.add(actorId);
        return null;
    }

    public ActionRejected started(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Entering && phase != RunPhase.Active)
                || entered.size() != members.size())
            return new ActionRejected(Rejection.RUN_MISMATCH);
        phase = RunPhase.Active;
        return null;
    }

    public ActionRejected completed(String id) {
        if (!Objects.equals(runId, id) || (phase != RunPhase.Active && phase != RunPhase.Completed))
            return new ActionRejected(Rejection.RUN_MISMATCH);
        phase = RunPhase.Completed;
        return null;
    }

    public PartyView view(String partyId) {
        return new PartyView(partyId, List.copyOf(members), members.isEmpty() ? "" : members.getFirst(),
                phase, runId);
    }
}
