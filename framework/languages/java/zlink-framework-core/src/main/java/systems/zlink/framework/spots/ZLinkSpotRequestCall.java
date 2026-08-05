package systems.zlink.framework.spots;

import java.time.Duration;
import java.util.Map;
import systems.zlink.framework.channels.ZLinkRequestCall;

public interface ZLinkSpotRequestCall extends ZLinkRequestCall {
    ZLinkSpotRequestCall instanceSpot();
    ZLinkSpotRequestCall instanceSpot(String stableType);
    ZLinkSpotRequestCall inMesh(String meshName);
    @Override
    ZLinkSpotRequestCall metadata(String key, String value);
    @Override
    ZLinkSpotRequestCall metadata(Map<String, String> metadata);
    @Override ZLinkSpotRequestCall timeout(Duration timeout);
}
