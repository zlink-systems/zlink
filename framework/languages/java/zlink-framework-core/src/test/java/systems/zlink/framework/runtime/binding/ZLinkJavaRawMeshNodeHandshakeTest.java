package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;

final class ZLinkJavaRawMeshNodeHandshakeTest {
    private static final Duration OBSERVATION_TIMEOUT = Duration.ofSeconds(2);

    @Test
    void sealedSelectedRouteDoesNotReceiveHello() {
        try (var context = Zlink.createContext();
                var local = new ZLinkJavaRawMeshNode(context, "mesh");
                var port = new ZLinkJavaRawServicePort(context);
                var peer = port.openRouter(RoutingId.from("java-1156-sealed-peer"))) {
            String endpoint = "inproc://java-1156-sealed-peer-" + System.nanoTime();
            peer.bind(endpoint);
            local.setRoutingId(RoutingId.from("java-1156-sealed-local"));
            local.setBind("inproc://java-1156-sealed-local-" + System.nanoTime());
            local.setPeerAdmissionSealGate(() -> true);
            local.start();
            RoutingId remoteRid = RoutingId.from("java-1156-sealed-peer");
            local.connectPeer(endpoint, remoteRid);
            assertNull(receiveCommand(port, peer), "sealed HELLO must not be submitted");
            assertTrue(local.isPeerTransportConnected(remoteRid));
        }
    }

    private static Integer receiveCommand(ZLinkJavaRawServicePort port, RouterSocket peer) {
        long deadline = System.nanoTime() + OBSERVATION_TIMEOUT.toNanos();
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0 || !port.waitForReadable(peer, Duration.ofNanos(remaining))) {
                return null;
            }
            try (var received = port.receive(peer)) {
                assertNotNull(received);
                if (received.frames().size() == 1 && received.frames().getFirst().length == 0) {
                    continue;
                }
                int command =
                        new ZLinkServiceM6AWireCodec()
                                .decodeHeader(received.frames().getFirst())
                                .command();
                if (command == ServiceWireConstants.COMMAND_HELLO) {
                    return command;
                }
            }
        }
    }
}
