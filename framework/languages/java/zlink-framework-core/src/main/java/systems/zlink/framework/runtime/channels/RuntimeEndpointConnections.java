package systems.zlink.framework.runtime.channels;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import systems.zlink.framework.configuration.ZLinkEndpointConnections;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendConnectableSocket;

final class RuntimeEndpointConnections implements ZLinkEndpointConnections {
    private final Runnable enable;
    private final List<String> endpoints;
    private ZLinkBackendConnectableSocket socket;

    RuntimeEndpointConnections(Runnable enable, List<String> endpoints) {
        this.enable = Objects.requireNonNull(enable, "enable");
        this.endpoints = Objects.requireNonNull(endpoints, "endpoints");
    }

    @Override
    public synchronized void connect(String endpoint) {
        String value = ChannelRegistration.requireEndpointValue(endpoint);
        enable.run();
        if (endpoints.contains(value)) {
            return;
        }
        if (socket != null) {
            socket.connect(value);
        }
        endpoints.add(value);
    }

    @Override
    public synchronized void disconnect(String endpoint) {
        String value = ChannelRegistration.requireEndpointValue(endpoint);
        if (!endpoints.contains(value)) {
            return;
        }
        if (socket != null) {
            socket.disconnect(value);
        }
        endpoints.remove(value);
    }

    @Override
    public synchronized List<String> listConnections() {
        return List.copyOf(endpoints);
    }

    synchronized void attach(ZLinkBackendConnectableSocket nextSocket) {
        if (socket != null) {
            throw new IllegalStateException("endpoint connections already have a runtime socket");
        }
        socket = Objects.requireNonNull(nextSocket, "nextSocket");
        List<String> configured = new ArrayList<>(endpoints);
        for (String endpoint : configured) {
            socket.connect(endpoint);
        }
    }

    synchronized void detach() {
        socket = null;
    }
}
