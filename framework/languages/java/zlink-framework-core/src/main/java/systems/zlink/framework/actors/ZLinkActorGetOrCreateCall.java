package systems.zlink.framework.actors;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkActorGetOrCreateCall {
    ZLinkActorGetOrCreateCall inMesh(String meshName);
    ZLinkActorGetOrCreateCall request(Object request);
    ZLinkActorGetOrCreateCall request(ZLinkMessage request);
    ZLinkActorGetOrCreateCall timeout(Duration timeout);
    CompletionStage<ZLinkActorCreateResult> submit();
    CompletionStage<ZLinkActorCreateResult> yield();
}
