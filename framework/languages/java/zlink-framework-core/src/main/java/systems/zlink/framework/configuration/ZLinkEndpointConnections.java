package systems.zlink.framework.configuration;

import java.util.List;

public interface ZLinkEndpointConnections {
    void connect(String endpoint);

    void disconnect(String endpoint);

    List<String> listConnections();
}
