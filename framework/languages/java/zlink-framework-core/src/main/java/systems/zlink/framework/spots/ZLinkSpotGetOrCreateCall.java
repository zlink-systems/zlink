package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkSpotGetOrCreateCall {
    ZLinkSpotGetOrCreateCall inMesh(String meshName);

    ZLinkSpotGetOrCreateCall request(Object request);

    ZLinkSpotGetOrCreateCall request(ZLinkMessage request);

    ZLinkSpotGetOrCreateCall timeout(Duration timeout);

    CompletionStage<ZLinkSpotCreateResult> submit();

    CompletionStage<ZLinkSpotCreateResult> yield();
}
