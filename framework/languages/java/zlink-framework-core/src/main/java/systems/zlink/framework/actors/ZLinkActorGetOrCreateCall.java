package systems.zlink.framework.actors;

import systems.zlink.framework.messaging.ZLinkMessage;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkActorGetOrCreateCall {
    ZLinkActorGetOrCreateCall inMesh(String meshName);

    ZLinkActorGetOrCreateCall request(Object request);

    ZLinkActorGetOrCreateCall request(ZLinkMessage request);

    ZLinkActorGetOrCreateCall timeout(Duration timeout);

    CompletionStage<ZLinkActorCreateResult> submit();

    CompletionStage<ZLinkActorCreateResult> yield();
}
