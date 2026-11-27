package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

public final class ZLinkActorJoinCompletionSealedException extends ZLinkFrameworkException {
    ZLinkActorJoinCompletionSealedException(String actorId) {
        super(ZLinkFrameworkErrorKind.UNAVAILABLE, "Actor Join completion failed: " + actorId);
    }
}
