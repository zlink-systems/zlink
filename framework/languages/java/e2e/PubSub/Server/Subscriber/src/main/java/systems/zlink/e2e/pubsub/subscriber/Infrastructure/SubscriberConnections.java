package Infrastructure;

import systems.zlink.e2e.pubsub.subscriber.Infrastructure;
import java.util.List;
import systems.zlink.framework.configuration.ZLinkEndpointConnections;

public final class SubscriberConnections {
    private volatile ZLinkEndpointConnections delegate;

    public void install(ZLinkEndpointConnections value) { delegate = value; }
    public void connect(String endpoint) { requireDelegate().connect(endpoint); }
    public void disconnect(String endpoint) { requireDelegate().disconnect(endpoint); }
    public List<String> list() { return requireDelegate().listConnections(); }

    private ZLinkEndpointConnections requireDelegate() {
        if (delegate == null) {
            throw new IllegalStateException("subscriber connections are not initialized");
        }
        return delegate;
    }
}
