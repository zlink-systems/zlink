package systems.zlink.crosslanguage.host;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

/** Actor hosted in {@link RelocationEntrySpot}; carries an application-state
 * byte array sized by the caller so relocation chunking (fixed 32 KiB
 * JoinEntrySpot chunk limit, spec 15:938) actually spans multiple chunks. */
public final class RelocationActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private byte[] applicationState = new byte[0];
    private int stateVersion;

    public RelocationActor(String actorId, ZLinkActorContext context) {
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
