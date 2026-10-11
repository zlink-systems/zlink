package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;

import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;

final class ZLinkChannelClientReceiveFlowTest {
    @Test
    void registeredClientDealerDoesNotReceiveHostPressure() {
        assertClientDealerDoesNotReceiveHostPressure(false);
    }

    @Test
    void discoveredClientDealerDoesNotReceiveHostPressure() {
        assertClientDealerDoesNotReceiveHostPressure(true);
    }

    private static void assertClientDealerDoesNotReceiveHostPressure(boolean discovered) {
        try (var queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
                        100,
                        0)) {
            List<ReceiveFlowState> states = new ArrayList<>();
            var dealer =
                    (ZLinkBackendDealerSocket)
                            Proxy.newProxyInstance(
                                    ZLinkBackendDealerSocket.class.getClassLoader(),
                                    new Class<?>[] {ZLinkBackendDealerSocket.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("setReceiveFlowState")) {
                                            states.add((ReceiveFlowState) arguments[0]);
                                        }
                                        if (method.getReturnType() == boolean.class) return false;
                                        if (method.getReturnType() == int.class) return 0;
                                        if (method.getReturnType() == long.class) return 0L;
                                        return null;
                                    });
            var registry = new ZLinkChannelSocketRegistry(queue);
            try {
                if (discovered) {
                    var descriptor =
                            new ZLinkClientServerServerDescriptor(
                                    "client",
                                    RoutingId.from("server"),
                                    1,
                                    1,
                                    "tcp://127.0.0.1:10001",
                                    100,
                                    ZLinkFrameworkRuntimeState.SERVING,
                                    "default",
                                    "local",
                                    1,
                                    Instant.EPOCH);
                    registry.addClientServerConnection("connection", descriptor, dealer);
                } else {
                    registry.registerClient("client", dealer);
                }
                try (var held =
                        queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join()) {
                    // Application job queue spec §6 excludes Client DEALER from receive flow.
                    assertEquals(List.of(), states);
                }
                assertEquals(List.of(), states);
            } finally {
                registry.closeAll();
            }
            assertEquals(List.of(), states);
        }
    }
}
