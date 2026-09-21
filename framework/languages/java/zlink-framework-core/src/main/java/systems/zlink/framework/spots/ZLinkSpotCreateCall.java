package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkSpotCreateCall {
    ZLinkSpotCreateCall inMesh(String meshName);

    ZLinkSpotCreateCall request(Object request);

    ZLinkSpotCreateCall request(ZLinkMessage request);

    ZLinkSpotCreateCall timeout(Duration timeout);

    CompletionStage<ZLinkSpotCreateResult> submit();

    CompletionStage<ZLinkSpotCreateResult> yield();
}
