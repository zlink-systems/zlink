package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.lang.reflect.InvocationHandler;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

final class ZLinkChannelReceiveFlowStateTest {
    @Test
    void pairedChannelSocketsReceiveAbsoluteStateBeforeExposureAndDeregisterOnClose() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
            100,
            0);
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry(queue);
        List<ReceiveFlowState> dealerStates = new ArrayList<>();
        List<ReceiveFlowState> serverStates = new ArrayList<>();
        List<ReceiveFlowState> routeStates = new ArrayList<>();
        ZLinkBackendDealerSocket dealer = socket(
            ZLinkBackendDealerSocket.class, dealerStates);
        ZLinkBackendRouterSocket server = socket(
            ZLinkBackendRouterSocket.class, serverStates);
        ZLinkBackendRouterSocket route = socket(
            ZLinkBackendRouterSocket.class, routeStates);

        sockets.registerClient("client", dealer);
        sockets.registerServer("server", RoutingId.from("server"), server);
        sockets.registerRouteRouter("route", route);
        assertEquals(List.of(ReceiveFlowState.RUNNING), dealerStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING), serverStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING), routeStates);

        ZLinkApplicationJobQueue.Permit permit =
            queue.acquire().toCompletableFuture().join();
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            dealerStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            serverStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            routeStates);

        sockets.closeAll();
        permit.close();
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            dealerStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            serverStates);
        assertEquals(List.of(ReceiveFlowState.RUNNING, ReceiveFlowState.PAUSED),
            routeStates);
    }

    @Test
    void initialReceiveFlowFailurePreventsClientSocketPublication() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
            100,
            0);
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry(queue);
        AtomicInteger flowCalls = new AtomicInteger();
        AtomicInteger closes = new AtomicInteger();
        ZLinkBackendDealerSocket dealer = (ZLinkBackendDealerSocket)
            Proxy.newProxyInstance(
                ZLinkBackendDealerSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendDealerSocket.class},
                (ignored, method, arguments) -> {
                    if (method.getName().equals("setReceiveFlowState")) {
                        flowCalls.incrementAndGet();
                        throw new ZlinkConfigException(ConfigResult.INVALID_STATE);
                    }
                    if (method.getName().equals("close")) {
                        closes.incrementAndGet();
                        return null;
                    }
                    return defaultValue(method.getReturnType());
                });

        assertThrows(ZlinkConfigException.class,
            () -> sockets.registerClient("client", dealer));
        sockets.closeAll();
        assertEquals(1, flowCalls.get());
        assertEquals(0, closes.get());
    }

    @Test
    void alternatePairedBackendCannotSilentlyIgnoreInitialReceiveFlowState() {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
            100,
            0);
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry(queue);
        ZLinkBackendDealerSocket dealer = (ZLinkBackendDealerSocket)
            Proxy.newProxyInstance(
                ZLinkBackendDealerSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendDealerSocket.class},
                (proxy, method, arguments) -> method.isDefault()
                    ? InvocationHandler.invokeDefault(proxy, method, arguments)
                    : defaultValue(method.getReturnType()));

        assertThrows(UnsupportedOperationException.class,
            () -> sockets.registerClient("client", dealer));
        assertEquals(1L,
            queue.pressureMetrics().flowStateConfigFailureCount());
    }

    @SuppressWarnings("unchecked")
    private static <T> T socket(Class<T> type, List<ReceiveFlowState> states) {
        return (T) Proxy.newProxyInstance(
            type.getClassLoader(),
            new Class<?>[] {type},
            (ignored, method, arguments) -> {
                if (method.getName().equals("setReceiveFlowState")) {
                    states.add((ReceiveFlowState) arguments[0]);
                    return null;
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == long.class) {
            return 0L;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == double.class) {
            return 0.0d;
        }
        if (type == float.class) {
            return 0.0f;
        }
        if (type == byte.class) {
            return (byte) 0;
        }
        if (type == short.class) {
            return (short) 0;
        }
        if (type == char.class) {
            return '\0';
        }
        return null;
    }
}
