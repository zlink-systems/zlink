package systems.zlink.framework.configuration;

import systems.zlink.contracts.core.RoutingId;

import java.util.List;

public interface ZLinkMeshPeerConnections {
    void connect(String endpoint);

    void connect(RoutingId expectedRoutingId, String endpoint);

    void disconnect(String endpoint);

    List<ZLinkMeshPeerConnection> listConnections();
}
