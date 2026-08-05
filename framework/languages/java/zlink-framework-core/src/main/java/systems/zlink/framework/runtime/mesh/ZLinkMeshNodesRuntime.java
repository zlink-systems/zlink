package systems.zlink.framework.runtime.mesh;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.function.Function;
import java.util.function.Consumer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public final class ZLinkMeshNodesRuntime implements AutoCloseable {
    private final List<ZLinkMeshNodeRuntime> nodes;

    private ZLinkMeshNodesRuntime(List<ZLinkMeshNodeRuntime> nodes) {
        this.nodes = List.copyOf(nodes);
    }

    public static ZLinkMeshNodesRuntime empty() {
        return new ZLinkMeshNodesRuntime(List.of());
    }

    public static ZLinkMeshNodesRuntime start(
        List<MeshNodeRegistration> registrations,
        ZLinkMeshBackendAdapter adapter,
        ZLinkBackendContext context) {
        return start(registrations, adapter, context, ignored -> null);
    }

    public static ZLinkMeshNodesRuntime start(
        List<MeshNodeRegistration> registrations,
        ZLinkMeshBackendAdapter adapter,
        ZLinkBackendContext context,
        Function<MeshNodeRegistration, Consumer<ZLinkMeshDispatchRecord>> receiverFactory) {
        List<ZLinkMeshNodeRuntime> started = new ArrayList<>();
        try {
            for (MeshNodeRegistration registration : registrations) {
                Consumer<ZLinkMeshDispatchRecord> receiver =
                    receiverFactory.apply(registration);
                ZLinkInboundDispatchBudget applicationDispatchBudget =
                    receiver instanceof ZLinkMeshApplicationReceiver applicationReceiver
                        ? applicationReceiver.applicationDispatchBudget()
                        : null;
                ZLinkMeshNodeRuntime runtime = ZLinkMeshNodeRuntime.start(
                    registration,
                    adapter,
                    context,
                    applicationDispatchBudget);
                started.add(runtime);
                if (receiver != null) {
                    if (receiver instanceof ZLinkMeshApplicationReceiver applicationReceiver) {
                        runtime.node().setApplicationReceiver(applicationReceiver);
                    }
                    runtime.node().startDispatch(receiver);
                }
            }
            return new ZLinkMeshNodesRuntime(started);
        } catch (RuntimeException failure) {
            closeReverse(started, failure);
            throw failure;
        }
    }

    public Map<String, ZLinkInternalMeshNode> nodesByName() {
        Map<String, ZLinkInternalMeshNode> result = new LinkedHashMap<>();
        for (ZLinkMeshNodeRuntime runtime : nodes) {
            result.put(runtime.node().name(), runtime.node());
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

    private static void closeReverse(
        List<ZLinkMeshNodeRuntime> nodes,
        RuntimeException failure) {
        for (int index = nodes.size() - 1; index >= 0; index--) {
            try {
                nodes.get(index).close();
            } catch (RuntimeException closeFailure) {
                failure.addSuppressed(closeFailure);
            }
        }
    }
}
