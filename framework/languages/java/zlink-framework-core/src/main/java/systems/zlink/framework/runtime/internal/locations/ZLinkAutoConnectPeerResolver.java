package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locations.ZLinkLocationRole;

import java.util.List;
import java.util.concurrent.CompletionStage;

public interface ZLinkAutoConnectPeerResolver {
    CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
            ZLinkAutoConnectType type, String meshName, ZLinkLocationRole role);
}
