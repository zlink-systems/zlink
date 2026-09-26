package systems.zlink.framework.runtime.mesh;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import java.util.function.Function;

/**
 * Owns the host's MeshNodes.
 *
 * <p>Each node is recorded here the moment the backend creates it, before it is configured or
 * started, so {@link #close()} releases a partial {@link #start} the same way it releases a running
 * set. The host records this object before calling {@code start}.
 */
public final class ZLinkMeshNodesRuntime implements AutoCloseable {
    //  Written only by start, which runs in the host's constructor before
    //  the host is published; read afterwards.
    private final List<ZLinkInternalMeshNode> nodes = new ArrayList<>();

    public void start(
            List<MeshNodeRegistration> registrations,
            ZLinkMeshBackendAdapter adapter,
            ZLinkBackendContext context,
            Function<MeshNodeRegistration, Consumer<ZLinkMeshDispatchRecord>> receiverFactory,
            boolean deferServiceReadyPublication,
            ZLinkApplicationJobQueue applicationJobQueue) {
        for (MeshNodeRegistration registration : registrations) {
            Consumer<ZLinkMeshDispatchRecord> receiver = receiverFactory.apply(registration);
            ZLinkInternalMeshNode node = adapter.createMeshNode(context, registration.meshName());
            nodes.add(node);
            ZLinkMeshNodeRuntime.start(
                    node,
                    registration,
                    deferServiceReadyPublication,
                    receiver,
                    applicationJobQueue);
        }
    }

    public Map<String, ZLinkInternalMeshNode> nodesByName() {
        Map<String, ZLinkInternalMeshNode> result = new LinkedHashMap<>();
        for (ZLinkInternalMeshNode node : nodes) {
            result.put(node.name(), node);
        }
        return Map.copyOf(result);
    }

    @Override
    public void close() {
        RuntimeException failure = null;
        for (int index = nodes.size() - 1; index >= 0; index--) {
            try {
                nodes.get(index).close();
            } catch (RuntimeException closeFailure) {
                if (failure == null) {
                    failure = closeFailure;
                } else {
                    failure.addSuppressed(closeFailure);
                }
            }
        }
        if (failure != null) {
            throw failure;
        }
    }
}
