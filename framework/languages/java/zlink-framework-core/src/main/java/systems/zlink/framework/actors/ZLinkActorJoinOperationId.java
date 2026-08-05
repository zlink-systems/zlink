package systems.zlink.framework.actors;

public record ZLinkActorJoinOperationId(long high, long low) {
    public ZLinkActorJoinOperationId {
        if (high == 0L && low == 0L) {
            throw new IllegalArgumentException("operation id must be non-zero");
        }
    }
}
