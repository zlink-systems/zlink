package systems.zlink.framework.spots;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkSpotGetOrCreateCall {
    ZLinkSpotGetOrCreateCall inMesh(String meshName);
    ZLinkSpotGetOrCreateCall request(Object request);
    ZLinkSpotGetOrCreateCall request(ZLinkMessage request);
    ZLinkSpotGetOrCreateCall timeout(Duration timeout);
    CompletionStage<ZLinkSpotCreateResult> submit();
    CompletionStage<ZLinkSpotCreateResult> yield();
}
