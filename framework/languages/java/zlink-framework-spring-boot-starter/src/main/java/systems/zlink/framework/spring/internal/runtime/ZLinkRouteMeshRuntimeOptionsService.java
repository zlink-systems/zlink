package systems.zlink.framework.spring.internal.runtime;

import java.util.Objects;
import java.util.function.Supplier;
import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshNodeRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;

/**
 * Exposes the core RouteMesh options through the Spring-managed lifecycle.
 */
public final class ZLinkRouteMeshRuntimeOptionsService
    implements ZLinkRouteMeshRuntimeOptions {
    private final Supplier<ZLinkRouteMeshRuntimeOptions> delegate;

    public ZLinkRouteMeshRuntimeOptionsService(ZLinkFrameworkLifecycle lifecycle) {
        this.delegate = Objects.requireNonNull(lifecycle, "lifecycle")::routeMeshRuntimeOptions;
    }

    ZLinkRouteMeshRuntimeOptionsService(
        Supplier<ZLinkRouteMeshRuntimeOptions> delegate) {
        this.delegate = Objects.requireNonNull(delegate, "delegate");
    }

    @Override
    public ZLinkMeshNodeRuntimeOptions meshNode(String meshName) {
        return delegate.get().meshNode(meshName);
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(
        String meshName,
        String channelName) {
        return delegate.get().channel(meshName, channelName);
    }

    @Override
    public ZLinkMeshPlacementRuntimeOptions mesh(String meshName) {
        return delegate.get().mesh(meshName);
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(String channelName) {
        return delegate.get().channel(channelName);
    }
}
