package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorEvent;
import systems.zlink.framework.runtime.internal.binding.spot.MeshMonitorStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;

import java.util.function.Supplier;

/** Pull monitor snapshot for the Framework-owned raw MeshNode. */
final class ZLinkJavaRawMeshMonitor implements MeshNodeMonitor {
    private final Supplier<MeshNodeStatus> status;
    private boolean closed;

    ZLinkJavaRawMeshMonitor(Supplier<MeshNodeStatus> status) {
        this.status = status;
    }

    @Override
    public MeshMonitorEvent recv(RecvFlags flags) {
        if (closed) {
            throw new IllegalStateException("MeshNode monitor is closed");
        }
        return null;
    }

    @Override
    public MeshMonitorStatus status() {
        MeshNodeStatus node = status.get();
        return new MeshMonitorStatus(
                node.state(),
                node.admittedPeerCount(),
                0,
                0,
                0,
                0,
                0,
                node.pendingApplicationMessages(),
                node.pendingInfrastructureMessages(),
                node.pendingBytes());
    }

    @Override
    public void close() {
        closed = true;
    }
}
