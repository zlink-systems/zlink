package systems.zlink.framework.actors;

import systems.zlink.framework.messaging.ZLinkMessage;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkActorCreateCall {
    ZLinkActorCreateCall inMesh(String meshName);

    ZLinkActorCreateCall request(Object request);

    ZLinkActorCreateCall request(ZLinkMessage request);

    ZLinkActorCreateCall timeout(Duration timeout);

    CompletionStage<ZLinkActorCreateResult> submit();

    CompletionStage<ZLinkActorCreateResult> yield();
}
