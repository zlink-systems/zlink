package systems.zlink.samples.zoneworld.server.zone.actors;

import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class PlayerActor implements ZLinkActor {
    private static final int COMPLETED_JOIN_RETENTION = 256;
    private final String actorId;
    private final ZLinkActorContext context;
    private final Set<ZLinkActorJoinOperationId> completedJoins = new LinkedHashSet<>();
    private int x;
    private int y;
    private String zoneId = "";
    private boolean isBot;
    private int dirX;
    private int dirY;
    private int pendingX;
    private int pendingY;
    private String pendingZone;
    private boolean pendingJoin;
    private JoinPurpose pendingPurpose = JoinPurpose.NONE;

    public PlayerActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public int x() {
        return x;
    }

    public int y() {
        return y;
    }

    public String zoneId() {
        return zoneId;
    }

    public boolean isBot() {
        return isBot;
    }

    public int dirX() {
        return dirX;
    }

    public int dirY() {
        return dirY;
    }

    public void prepareEntry(int x, int y, boolean bot, int dirX, int dirY) {
        this.x = x;
        this.y = y;
        this.isBot = bot;
        this.dirX = dirX;
        this.dirY = dirY;
        this.pendingX = x;
        this.pendingY = y;
        this.pendingZone = ZoneWorldSpec.zoneOf(x, y);
        this.pendingJoin = true;
        this.pendingPurpose = bot ? JoinPurpose.INITIAL_BOT : JoinPurpose.INITIAL_HUMAN;
    }

    public void prepareMove(int x, int y, String zoneId) {
        this.pendingX = x;
        this.pendingY = y;
        this.pendingZone = zoneId;
        this.pendingJoin = true;
        this.pendingPurpose = JoinPurpose.ZONE_CHANGE;
    }

    public void prepareCrashProbe(int x, int y, String zoneId) {
        prepareMove(x, y, zoneId);
        pendingPurpose = JoinPurpose.CRASH_PROBE;
    }

    public int pendingX() {
        return pendingX;
    }

    public int pendingY() {
        return pendingY;
    }

    public String pendingZone() {
        return pendingZone;
    }

    public boolean pendingJoin() {
        return pendingJoin;
    }

    public String pendingPurpose() {
        return pendingPurpose.name();
    }

    public List<ZLinkActorJoinOperationId> completedJoins() {
        return List.copyOf(completedJoins);
    }

    public void reverseDirection() {
        dirX = -dirX;
        dirY = -dirY;
    }

    public void updatePosition(int x, int y) {
        this.x = x;
        this.y = y;
    }

    public void applyAtZone(int x, int y, String zoneId, boolean bot) {
        this.x = x;
        this.y = y;
        this.zoneId = zoneId;
        this.isBot = bot;
    }

    public void restoreState(
        int x,
        int y,
        String zoneId,
        boolean bot,
        int dirX,
        int dirY,
        int pendingX,
        int pendingY,
        String pendingZone,
        boolean pendingJoin,
        String pendingPurpose,
        List<ZLinkActorJoinOperationId> completedJoins) {
        this.x = x;
        this.y = y;
        this.zoneId = zoneId;
        this.isBot = bot;
        this.dirX = dirX;
        this.dirY = dirY;
        this.pendingX = pendingX;
        this.pendingY = pendingY;
        this.pendingZone = pendingZone;
        this.pendingJoin = pendingJoin;
        this.pendingPurpose = JoinPurpose.valueOf(pendingPurpose);
        this.completedJoins.clear();
        completedJoins.forEach(this::rememberJoin);
    }

    public CompletionStage<Void> send(Object message) {
        if (isBot) return CompletableFuture.completedFuture(null);
        return context.boundSession().send(message).submit();
    }

    @Override
    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        ZLinkActorJoinOperationId operationId = completion instanceof ZLinkActorJoinCompletion.Accepted accepted
            ? accepted.operationId()
            : completion instanceof ZLinkActorJoinCompletion.Rejected rejected
                ? rejected.operationId()
                : ((ZLinkActorJoinCompletion.Failed) completion).operationId();
        if (completedJoins.contains(operationId)) return CompletableFuture.completedFuture(null);
        rememberJoin(operationId);

        pendingJoin = false;
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            Messages.EnterZoneRes reply = accepted.reply().decode(Messages.EnterZoneRes.class);
            String joinedZone = reply.zoneId().isBlank()
                ? pendingZone == null || pendingZone.isBlank()
                    ? ZoneWorldSpec.zoneOf(pendingX, pendingY)
                    : pendingZone
                : reply.zoneId();
            applyAtZone(pendingX, pendingY, joinedZone, isBot);
            pendingZone = null;
            if (pendingPurpose == JoinPurpose.INITIAL_HUMAN) {
                pendingPurpose = JoinPurpose.NONE;
                return send(new Messages.JoinWorldNotify(actorId, joinedZone, x, y, null));
            }
            if (pendingPurpose == JoinPurpose.CRASH_PROBE) {
                pendingPurpose = JoinPurpose.NONE;
                return send(new Messages.CrashRelocationProbeRes(null));
            }
            pendingPurpose = JoinPurpose.NONE;
            return CompletableFuture.completedFuture(null);
        }

        String reason = completion instanceof ZLinkActorJoinCompletion.Rejected rejected
            ? rejected.reply().decode(Messages.EnterZoneRes.class).error()
            : mapFailure(((ZLinkActorJoinCompletion.Failed) completion).kind().name());
        pendingZone = null;
        if (pendingPurpose == JoinPurpose.INITIAL_HUMAN) {
            pendingPurpose = JoinPurpose.NONE;
            return send(new Messages.JoinWorldNotify(
                actorId, ZoneWorldSpec.zoneOf(pendingX, pendingY), pendingX, pendingY, reason));
        }
        if (pendingPurpose == JoinPurpose.CRASH_PROBE) {
            pendingPurpose = JoinPurpose.NONE;
            return send(new Messages.CrashRelocationProbeRes(reason));
        }
        pendingPurpose = JoinPurpose.NONE;
        if (!isBot) return send(new Messages.MoveRejectedNotify(reason, x, y));
        reverseDirection();
        return CompletableFuture.completedFuture(null);
    }

    private static String mapFailure(String kind) {
        return switch (kind) {
            case "UNAVAILABLE", "DEADLINE_EXCEEDED", "SHUTTING_DOWN" -> "Unavailable";
            case "NOT_FOUND" -> "NotFound";
            case "CAPACITY_EXCEEDED" -> "CapacityExceeded";
            case "REJECTED" -> "Rejected";
            default -> "InternalFailure";
        };
    }

    private void rememberJoin(ZLinkActorJoinOperationId operationId) {
        if (!completedJoins.add(operationId)) return;
        while (completedJoins.size() > COMPLETED_JOIN_RETENTION) {
            completedJoins.remove(completedJoins.iterator().next());
        }
    }

    private enum JoinPurpose { NONE, INITIAL_HUMAN, INITIAL_BOT, ZONE_CHANGE, CRASH_PROBE }
}
