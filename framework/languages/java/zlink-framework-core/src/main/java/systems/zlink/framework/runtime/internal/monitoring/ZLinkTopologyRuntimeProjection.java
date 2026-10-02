package systems.zlink.framework.runtime.internal.monitoring;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

import java.util.Optional;

public final class ZLinkTopologyRuntimeProjection {
    private ZLinkTopologyRuntimeProjection() {}

    public static ZLinkTopologyState hostState(ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> ZLinkTopologyState.STARTING;
            case SERVING -> ZLinkTopologyState.READY;
            case RELOCATING, RELOCATED, DRAINING -> ZLinkTopologyState.STOPPING;
            case STOPPED -> ZLinkTopologyState.STOPPED;
            case ERROR -> ZLinkTopologyState.FAILED;
        };
    }

    public static ZLinkMeshPeerSnapshot snapshot(
            RoutingId nodeRid,
            ZLinkFrameworkRuntimeState hostState,
            boolean ready,
            boolean connecting,
            ZLinkTopologyReason unavailableReason) {
        if (hostState == ZLinkFrameworkRuntimeState.RELOCATING
                || hostState == ZLinkFrameworkRuntimeState.RELOCATED
                || hostState == ZLinkFrameworkRuntimeState.DRAINING) {
            return new ZLinkMeshPeerSnapshot(
                    nodeRid, ZLinkPeerState.DRAINING, Optional.of(ZLinkTopologyReason.DRAINING));
        }
        if (ready) {
            return new ZLinkMeshPeerSnapshot(nodeRid, ZLinkPeerState.READY, Optional.empty());
        }
        return new ZLinkMeshPeerSnapshot(
                nodeRid,
                connecting ? ZLinkPeerState.CONNECTING : ZLinkPeerState.NOT_CONNECTED,
                Optional.of(unavailableReason));
    }
}
