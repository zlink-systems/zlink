package systems.zlink.framework.actors;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkActorCreateCall {
    ZLinkActorCreateCall inMesh(String meshName);
    ZLinkActorCreateCall request(Object request);
    ZLinkActorCreateCall request(ZLinkMessage request);
    ZLinkActorCreateCall timeout(Duration timeout);
    CompletionStage<ZLinkActorCreateResult> submit();
    CompletionStage<ZLinkActorCreateResult> yield();
}
