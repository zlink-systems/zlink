package systems.zlink.framework.runtime.internal.locations;

import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.ZLinkLocationRole;

public interface ZLinkAutoConnectPeerResolver {
    CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
        ZLinkAutoConnectType type,
        String meshName,
        ZLinkLocationRole role);
}
