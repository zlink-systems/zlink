package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Flow;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshNodeRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.monitoring.ZLinkMeshChannelSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkClientServerStatus;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkFanoutStatus;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

final class RuntimeMonitoringContractTest {
    @Test
    void routeMeshRuntimeMatchesExactPublicMethodShape() throws Exception {
        assertEquals(
            ZLinkMeshNodeSnapshot.class,
            ZLinkRouteMeshRuntime.class
                .getMethod("snapshot", String.class)
                .getReturnType());
        assertEquals(
            Flow.Publisher.class,
            ZLinkRouteMeshRuntime.class
                .getMethod("observe", String.class, int.class)
                .getReturnType());
        assertEquals(
            boolean.class,
            ZLinkRouteMeshRuntime.class
                .getMethod("isReady", String.class)
                .getReturnType());
    }

    @Test
    void hostAndTopologyRuntimeMatchMinimalPublicContract() throws Exception {
        assertEquals(
            ZLinkFrameworkRuntimeStatus.class,
            ZLinkFrameworkRuntime.class.getMethod("status").getReturnType());
        assertEquals(
            Flow.Publisher.class,
            ZLinkFrameworkRuntime.class.getMethod("observe").getReturnType());
        assertEquals(
            ZLinkClientServerStatus.class,
            ZLinkClientServerRuntime.class
                .getMethod("snapshot", String.class)
                .getReturnType());
        assertEquals(
            ZLinkFanoutStatus.class,
            ZLinkFanoutRuntime.class
                .getMethod("snapshot", String.class)
                .getReturnType());
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkRuntimeQuery"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkSocketEvent"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeSnapshot"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkMeshNodeState"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkRuntimeErrorSink"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkRuntimeErrorEvent"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.monitoring.ZLinkRuntimeErrorEventKind"));
    }

    @Test
    void routeMeshRuntimeOptionsMatchExactPublicMethodShape() throws Exception {
        assertEquals(
            ZLinkMeshNodeRuntimeOptions.class,
            ZLinkRouteMeshRuntimeOptions.class
                .getMethod("meshNode", String.class)
                .getReturnType());
        assertEquals(
            ZLinkMeshChannelRuntimeOptions.class,
            ZLinkRouteMeshRuntimeOptions.class
                .getMethod("channel", String.class, String.class)
                .getReturnType());
    }

    @Test
    void meshNodeSnapshotDefensivelyCopiesCollectionInputs() {
        ArrayList<ZLinkMeshChannelSnapshot> channels = new ArrayList<>(
            List.of(new ZLinkMeshChannelSnapshot("channel", true, 1)));
        ZLinkMeshNodeSnapshot snapshot = new ZLinkMeshNodeSnapshot(
            "mesh",
            ZLinkTopologyState.READY,
            true,
            0,
            channels,
            List.of(),
            new ZLinkPlacementSnapshot(true, 2, 3, java.util.Optional.empty()),
            1,
            Instant.now());

        channels.clear();

        assertEquals(1, snapshot.channels().size());
        assertEquals(2, snapshot.placement().activeActorCount());
        assertEquals(3, snapshot.placement().activeSpotCount());
        assertThrows(
            UnsupportedOperationException.class,
            () -> snapshot.channels().add(
                new ZLinkMeshChannelSnapshot("other", false, 0)));
    }

    @Test
    void publicTopologyStatusDoesNotExposeDiscoveryInternals() {
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkMeshNodeSnapshot.class.getMethod("descriptorRevision"));
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkMeshNodeSnapshot.class.getMethod("endpoint"));
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkMeshPeerSnapshot.class.getMethod("descriptorRevision"));
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkMeshPeerSnapshot.class.getMethod("endpoint"));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkTopologyState.valueOf("DRAINED"));
    }
}
