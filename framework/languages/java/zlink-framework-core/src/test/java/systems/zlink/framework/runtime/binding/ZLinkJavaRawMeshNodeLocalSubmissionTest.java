package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.time.Duration;
import java.util.List;
import java.util.Queue;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkJavaRawMeshNodeLocalSubmissionTest {
    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void rejectedLocalSubmissionReleasesMessages(boolean rejectTurn) throws Exception {
        var copied = new AtomicReference<List<Message>>();
        var laneReference = new AtomicReference<ZLinkStateLane>();
        var rejected = new RejectedExecutionException("source executor rejected submission");
        var lane =
                new ZLinkStateLane(
                        command -> {
                            try {
                                copied.set(
                                        capturedMessages(mailbox(laneReference.get()).element()));
                            } catch (ReflectiveOperationException failure) {
                                throw new AssertionError(failure);
                            }
                            if (rejectTurn) throw rejected;
                            command.run();
                        });
        laneReference.set(lane);
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var packet = Message.from("packet");
                var body = Message.from("payload")) {
            var originalLane = node.sourceTransportLane();
            var laneField = ZLinkJavaRawMeshNode.class.getDeclaredField("transportStateLane");
            laneField.setAccessible(true);
            var descriptorField = ZLinkJavaRawMeshNode.class.getDeclaredField("localDescriptor");
            descriptorField.setAccessible(true);
            var rid = RoutingId.from("local");
            descriptorField.set(
                    node,
                    new ZLinkServiceNodeDescriptor(
                            "mesh",
                            rid,
                            1,
                            1,
                            "inproc://local-submission",
                            List.of(),
                            ZLinkServiceNodeDescriptor.State.SERVING,
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            0,
                            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                            ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                            0,
                            1,
                            0,
                            0,
                            0));
            var dispatch =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "dispatchLocalInstanceSpot",
                            ZLinkServiceM6BWireCodec.InstanceRoute.class,
                            String.class,
                            String.class,
                            byte[].class,
                            List.class,
                            boolean.class,
                            long.class,
                            CompletableFuture.class);
            dispatch.setAccessible(true);
            laneField.set(node, lane);
            try {
                CompletableFuture.runAsync(
                                ZLinkBlockingCalls.infrastructureTask(
                                        () -> {
                                            Callable<Object> invoke =
                                                    () ->
                                                            dispatch.invoke(
                                                                    node,
                                                                    new ZLinkServiceM6BWireCodec
                                                                            .InstanceColdActivation(
                                                                            rid,
                                                                            1,
                                                                            "spot",
                                                                            "mesh",
                                                                            "type",
                                                                            "v1",
                                                                            System
                                                                                            .currentTimeMillis()
                                                                                    + 10_000),
                                                                    "type",
                                                                    null,
                                                                    null,
                                                                    List.of(packet, body),
                                                                    true,
                                                                    rejectTurn
                                                                            ? System.nanoTime()
                                                                                    + Duration
                                                                                            .ofSeconds(
                                                                                                    10)
                                                                                            .toNanos()
                                                                            : 0,
                                                                    null);
                                            if (rejectTurn) {
                                                var failure =
                                                        assertThrows(
                                                                InvocationTargetException.class,
                                                                invoke::call);
                                                assertEquals(rejected, failure.getCause());
                                            } else {
                                                var stage =
                                                        assertInstanceOf(
                                                                CompletionStage.class,
                                                                assertDoesNotThrow(invoke::call));
                                                var failure =
                                                        assertThrows(
                                                                CompletionException.class,
                                                                () ->
                                                                        stage.toCompletableFuture()
                                                                                .join());
                                                assertInstanceOf(
                                                        TimeoutException.class, failure.getCause());
                                            }
                                        }))
                        .join();
                assertEquals(2, copied.get().size());
                for (Message message : copied.get()) {
                    assertThrows(
                            IllegalStateException.class,
                            message::refCount,
                            "a rejected submission must release its copied messages");
                }
                assertEquals("packet", packet.toUtf8String());
                assertEquals("payload", body.toUtf8String());
            } finally {
                laneField.set(node, originalLane);
                Message.closeAll(copied.get());
                mailbox(lane).clear();
                lane.closeAsync().toCompletableFuture().join();
            }
        }
    }

    private static Queue<?> mailbox(ZLinkStateLane lane) throws ReflectiveOperationException {
        var field = ZLinkStateLane.class.getDeclaredField("mailbox");
        field.setAccessible(true);
        return (Queue<?>) field.get(lane);
    }

    // Inspect the queued supplier before the executor rejects it: these are the
    // actual owned frames, rather than the caller's still-valid input messages.
    private static List<Message> capturedMessages(Object captured)
            throws ReflectiveOperationException {
        if (captured instanceof List<?> list
                && !list.isEmpty()
                && list.getFirst() instanceof Message) {
            for (Object part : list) assertInstanceOf(Message.class, part);
            @SuppressWarnings("unchecked")
            var messages = (List<Message>) list;
            return messages;
        }
        if (captured == null || !captured.getClass().isSynthetic()) return null;
        for (var field : captured.getClass().getDeclaredFields()) {
            if (Modifier.isStatic(field.getModifiers())) continue;
            field.setAccessible(true);
            var messages = capturedMessages(field.get(captured));
            if (messages != null) return messages;
        }
        return null;
    }
}
