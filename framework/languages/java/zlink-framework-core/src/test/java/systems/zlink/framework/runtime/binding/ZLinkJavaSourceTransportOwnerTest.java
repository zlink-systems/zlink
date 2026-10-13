package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkJavaSourceTransportOwnerTest {
    @Test
    void receiveManagementReusesNodeOwnerTurn() throws Exception {
        try (var context = systems.zlink.contracts.core.Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "management-owner")) {
            var field = ZLinkJavaRawMeshNode.class.getDeclaredField("liveness");
            field.setAccessible(true);
            var liveness =
                    (systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry)
                            field.get(node);
            var tick = ZLinkJavaRawMeshNode.class.getDeclaredMethod("tickLiveness", long.class);
            tick.setAccessible(true);
            node.sourceTransportLane()
                    .awaitTurn(
                            () -> {
                                try {
                                    tick.invoke(node, System.nanoTime());
                                } catch (ReflectiveOperationException failure) {
                                    throw new java.util.concurrent.CompletionException(failure);
                                }
                                return null;
                            });
            assertEquals(0, liveness.size());
        }
    }

    @Test
    void nodeTurnStartsNativeSubmissionOnTheSameOwner() throws Exception {
        var nativeOwner = new AtomicReference<ZLinkStateLane>();
        RouterSocket router =
                (RouterSocket)
                        Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            case "equals" -> proxy == args[0];
                                            case "setRoutingId", "close" -> null;
                                            case "send" -> {
                                                nativeOwner.set(ZLinkStateLane.current());
                                                throw new ZlinkSubmitException(
                                                        SubmitResult.NOT_CONNECTED);
                                            }
                                            default -> throw new AssertionError(method.getName());
                                        });
        Context nativeContext =
                (Context)
                        Proxy.newProxyInstance(
                                Context.class.getClassLoader(),
                                new Class<?>[] {Context.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "createRouterSocket" -> router;
                                            case "close", "shutdown" -> null;
                                            default -> throw new AssertionError(method.getName());
                                        });
        var context = new ZLinkJavaContext(nativeContext);
        try (var node =
                (ZLinkJavaRawMeshNode)
                        new ZLinkJavaMeshBackendAdapter().createMeshNode(context, "owner-mesh")) {
            var field = ZLinkJavaRawMeshNode.class.getDeclaredField("port");
            field.setAccessible(true);
            var port = (ZLinkJavaRawServicePort) field.get(node);
            var socket = port.openRouter(RoutingId.from("owner-source"));
            var ownerField = ZLinkJavaRawMeshNode.class.getDeclaredField("transportStateLane");
            ownerField.setAccessible(true);
            var selectedOwner = (ZLinkStateLane) ownerField.get(node);
            selectedOwner
                    .runNowOrQueue(
                            () -> {
                                assertThrows(
                                        ZlinkSubmitException.class,
                                        () ->
                                                port.sendMessages(
                                                        socket,
                                                        RoutingId.from("owner-target"),
                                                        List.of(Message.from(new byte[] {1}))));
                                return null;
                            })
                    .toCompletableFuture()
                    .join();
            assertSame(selectedOwner, nativeOwner.get());
        }
    }
}
