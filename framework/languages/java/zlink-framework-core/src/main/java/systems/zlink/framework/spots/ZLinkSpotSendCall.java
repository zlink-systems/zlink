package systems.zlink.framework.spots;

import java.util.Map;
import systems.zlink.framework.channels.ZLinkSendCall;

public interface ZLinkSpotSendCall extends ZLinkSendCall {
    ZLinkSpotSendCall instanceSpot();
    ZLinkSpotSendCall instanceSpot(String stableType);
    ZLinkSpotSendCall inMesh(String meshName);
    @Override
    ZLinkSpotSendCall metadata(String key, String value);
    @Override
    ZLinkSpotSendCall metadata(Map<String, String> metadata);
}
