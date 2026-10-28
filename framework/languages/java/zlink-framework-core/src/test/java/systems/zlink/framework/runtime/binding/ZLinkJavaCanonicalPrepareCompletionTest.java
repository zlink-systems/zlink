package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistryTesting;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaCanonicalPrepareCompletionTest {
    private static final RoutingId LOCAL = RoutingId.from("prepare-source");
    private static final RoutingId REMOTE = RoutingId.from("prepare-target");

    @Test
    void localAdmissionDoesNotWaitForReadyAndUsesTheFinalRegistryReceiver() throws Exception {
        try (var fixture = new Fixture()) {
            var physical = new CompletableFuture<byte[]>();
            fixture.node.setCanonicalRelocationControlHandler(
                    (source, sequence, command) -> {
                        assertEquals(1, fixture.operations.pendingCount());
                        return physical;
                    });
            var admitted = new CompletableFuture<Void>();
            var reply =
                    fixture.node
                            .requestCanonicalRelocationPrepare(
                                    LOCAL, command(40), Duration.ofSeconds(2), admitted)
                            .toCompletableFuture();
            assertTrue(admitted.isDone(), "physical acceptance must precede READY");
            admitted.get(1, TimeUnit.SECONDS);
            assertFalse(reply.isDone());
            assertSame(reply, fixture.pendingReceiver());
            physical.complete(command(30));
            assertArrayEquals(command(30), reply.get(1, TimeUnit.SECONDS));
            assertEquals(0, fixture.operations.pendingCount());
        }
    }

    @Test
    void localPendingPrepareUsesTheRequestOwnerDeadline() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.node.setCanonicalRelocationControlHandler(
                    (source, sequence, command) -> new CompletableFuture<>());
            var admitted = new CompletableFuture<Void>();
            var reply =
                    fixture.node
                            .requestCanonicalRelocationPrepare(
                                    LOCAL, command(40), Duration.ofMillis(20), admitted)
                            .toCompletableFuture();
            admitted.get(1, TimeUnit.SECONDS);
            var failure =
                    assertThrows(ExecutionException.class, () -> reply.get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(0, fixture.operations.pendingCount());
        }
    }

    @Test
    void remoteReplyDecodesDirectlyIntoItsSingleRegisteredReceiver() throws Exception {
        try (var fixture = new Fixture(true)) {
            assertTrue(fixture.node.observePeerRoute(REMOTE).ready());
            var admitted = new CompletableFuture<Void>();
            var reply =
                    fixture.node
                            .requestCanonicalRelocationPrepare(
                                    REMOTE, command(40), Duration.ofSeconds(2), admitted)
                            .toCompletableFuture();
            reply.whenComplete(
                    (value, failure) -> {
                        if (failure != null) fixture.submitted.completeExceptionally(failure);
                    });
            fixture.submitted.get(1, TimeUnit.SECONDS);
            admitted.get(1, TimeUnit.SECONDS);
            assertSame(
                    reply,
                    fixture.pendingReceiver(),
                    "canonical decode must use the registered typed receiver");
            assertEquals(1, fixture.operations.pendingCount());
            var payload = Message.from(command(30));
            fixture.physicalReply.complete(List.of(payload));
            assertArrayEquals(command(30), reply.get(1, TimeUnit.SECONDS));
            assertTrue(payload.empty());
            assertEquals(0, fixture.operations.pendingCount());
        }
    }

    private static final class Fixture implements AutoCloseable {
        final List<Message> requestParts = new ArrayList<>();
        final CompletableFuture<List<Message>> physicalReply = new CompletableFuture<>();
        final CompletableFuture<Void> submitted = new CompletableFuture<>();
        final Context context;
        final RouterSocket router;
        final ZLinkJavaRawMeshNode node;
        final ZLinkServiceOperationRegistry operations;

        Fixture() throws Exception {
            this(false);
        }

        Fixture(boolean remote) throws Exception {
            RequestSubmission submission =
                    (RequestSubmission)
                            Proxy.newProxyInstance(
                                    RequestSubmission.class.getClassLoader(),
                                    new Class<?>[] {RequestSubmission.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "result" -> SubmitResult.OK;
                                                case "reply" -> physicalReply;
                                                default -> throw new AssertionError(method);
                                            });
            RequestSubmitOperation operation =
                    (RequestSubmitOperation)
                            Proxy.newProxyInstance(
                                    RequestSubmitOperation.class.getClassLoader(),
                                    new Class<?>[] {RequestSubmitOperation.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("message")) {
                                            requestParts.add((Message) args[0]);
                                            return proxy;
                                        }
                                        if (method.getName().equals("timeout")) return proxy;
                                        if (method.getName().equals("submit")) {
                                            Message.closeAll(requestParts);
                                            submitted.complete(null);
                                            return submission;
                                        }
                                        throw new AssertionError(method);
                                    });
            RequestOperation request = message -> operation.message(message);
            router =
                    (RouterSocket)
                            Proxy.newProxyInstance(
                                    RouterSocket.class.getClassLoader(),
                                    new Class<?>[] {RouterSocket.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "hashCode" -> System.identityHashCode(proxy);
                                                case "equals" -> proxy == args[0];
                                                case "setRoutingId", "close" -> null;
                                                case "request" -> request;
                                                default -> throw new AssertionError(method);
                                            });
            context =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("createRouterSocket"))
                                            return router;
                                        throw new AssertionError(method);
                                    });
            var port = new ZLinkJavaRawServicePort(context);
            var topology = new ZLinkServiceTopologyRegistry(descriptor(LOCAL), port.stateLane());
            if (remote) {
                port.openRouter(LOCAL);
                topology.admit(descriptor(REMOTE), "1", 1);
            }
            node =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "prepare-completion",
                            port,
                            remote ? router : null,
                            null,
                            remote ? topology : null,
                            remote ? Map.of(REMOTE, 1L) : Map.of(),
                            remote ? Map.of(REMOTE, "1") : Map.of(),
                            Runnable::run);
            node.setRoutingId(LOCAL);
            operations = node.operationRegistry();
        }

        Object pendingReceiver() {
            assertEquals(1, operations.pendingCount());
            return ZLinkServiceOperationRegistryTesting.pendingReceiver(operations);
        }

        @Override
        public void close() {
            node.close();
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "prepare-completion",
                rid,
                1,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                1,
                0,
                0,
                0);
    }

    private static byte[] command(int command) throws Exception {
        for (Path current = Path.of("").toAbsolutePath();
                current != null;
                current = current.getParent()) {
            for (String relative :
                    List.of(
                            "framework/runtime/protocol/golden/relocation-control-v1.json",
                            "runtime/protocol/golden/relocation-control-v1.json")) {
                var file = current.resolve(relative);
                if (!Files.isRegularFile(file)) continue;
                for (var entry :
                        new ObjectMapper().readTree(Files.readString(file)).path("canonical")) {
                    if (entry.path("command").asInt() == command)
                        return HexFormat.of().parseHex(entry.path("hex").asText());
                }
            }
        }
        throw new IllegalStateException("canonical relocation command was not found: " + command);
    }
}
