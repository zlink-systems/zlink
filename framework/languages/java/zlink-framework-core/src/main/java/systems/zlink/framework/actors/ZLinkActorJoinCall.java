package systems.zlink.framework.actors;

import java.time.Duration;

public interface ZLinkActorJoinCall {
    ZLinkActorJoinCall timeout(Duration timeout);

    void defer();
}
