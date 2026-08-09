package systems.zlink.samples.zoneworld.server.zone.actors;

import java.util.HashSet;
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
    private final String actorId;
    private final ZLinkActorContext context;
    private final Set<ZLinkActorJoinOperationId> completedJoins = new HashSet<>();
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
    }

    public void prepareMove(int x, int y, String zoneId) {
        this.pendingX = x;
        this.pendingY = y;
        this.pendingZone = zoneId;
        this.pendingJoin = true;
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
        boolean pendingJoin) {
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
        if (!completedJoins.add(operationId)) return CompletableFuture.completedFuture(null);

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
            if (!isBot) {
                return send(new Messages.ZoneChangedNotify(actorId, joinedZone));
            }
            return CompletableFuture.completedFuture(null);
        }

        String reason = completion instanceof ZLinkActorJoinCompletion.Rejected rejected
            ? rejected.reply().decode(Messages.EnterZoneRes.class).error()
            : ((ZLinkActorJoinCompletion.Failed) completion).kind().name();
        pendingZone = null;
        if (!isBot) return send(new Messages.MoveRejectedNotify(reason, x, y));
        return CompletableFuture.completedFuture(null);
    }
}
