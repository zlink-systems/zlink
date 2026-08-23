package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;

/** Actor hosted in {@link RelocationEntrySpot}; carries an application-state
 * byte array sized by the caller so relocation chunking (fixed 32 KiB
 * JoinEntrySpot chunk limit, spec 15:938) actually spans multiple chunks. */
public final class RelocationActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private final EventSink sink;
    private byte[] applicationState = new byte[0];
    private int stateVersion;

    public RelocationActor(String actorId, ZLinkActorContext context, EventSink sink) {
        this.actorId = actorId;
        this.context = context;
        this.sink = sink;
    }

    /** Terminal fate of a deferred joinSpot(): the only place a canonical
     * actorJoin failure surfaces on the source side. */
    @Override
    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (sink != null) {
            switch (completion) {
                case ZLinkActorJoinCompletion.Accepted accepted -> sink.append(
                    "user-spot-join-completed|status=accepted|actor=" + actorId);
                case ZLinkActorJoinCompletion.Rejected rejected -> sink.append(
                    "user-spot-join-completed|status=rejected|actor=" + actorId);
                case ZLinkActorJoinCompletion.Failed failed -> sink.append(
                    "user-spot-join-failed|kind=" + failed.kind().name() + "|actor=" + actorId);
            }
        }
        return CompletableFuture.completedFuture(null);
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public byte[] applicationState() {
        return applicationState;
    }

    public void setApplicationState(byte[] applicationState) {
        this.applicationState = applicationState;
    }

    public int stateVersion() {
        return stateVersion;
    }

    public void setStateVersion(int stateVersion) {
        this.stateVersion = stateVersion;
    }
}
