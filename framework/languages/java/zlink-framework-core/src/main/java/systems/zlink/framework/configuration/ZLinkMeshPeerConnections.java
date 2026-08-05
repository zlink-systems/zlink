package systems.zlink.framework.configuration;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;

public interface ZLinkMeshPeerConnections {
    void connect(String endpoint);

    void connect(RoutingId expectedRoutingId, String endpoint);

    void disconnect(String endpoint);

    List<ZLinkMeshPeerConnection> listConnections();
}
