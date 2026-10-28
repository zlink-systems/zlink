package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLaneTestProbe;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeoutException;

final class ZLinkJavaRawMeshNodeLocalSubmissionTest {
    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void rejectedLocalSubmissionReleasesMessages(boolean rejectTurn) throws Exception {
        var rejected = new RejectedExecutionException("source executor rejected submission");
        var submissionFinished = new java.util.concurrent.atomic.AtomicBoolean();
        var mailbox =
                new ZLinkStateLaneTestProbe.Mailbox(
                        command -> {
                            if (rejectTurn && !submissionFinished.get()) throw rejected;
                            command.run();
                        });
        var lane = mailbox.lane();
        var rid = RoutingId.from("local");
        var descriptor =
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
                        0);

        var context =
                (Context)
                        Proxy.newProxyInstance(
                                Context.class.getClassLoader(),
                                new Class<?>[] {Context.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("close")) return null;
                                    throw new AssertionError(
                                            "unexpected context call: " + method.getName());
                                });
        try (context;
                var node =
                        ZLinkJavaRawMeshNodeTesting.create(
                                "mesh",
                                new ZLinkJavaRawServicePort(context, lane),
                                null,
                                descriptor,
                                null,
                                Map.of(),
                                Runnable::run);
                var packet = Message.from("packet");
                var body = Message.from("payload")) {
            var submission =
                    node
                    .new LocalInstanceSubmission(
                            new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                                    rid,
                                    1,
                                    "spot",
                                    "mesh",
                                    "type",
                                    "v1",
                                    System.currentTimeMillis() + 10_000),
                            null,
                            null,
                            List.of(packet, body),
                            true,
                            rejectTurn ? System.nanoTime() + Duration.ofSeconds(10).toNanos() : 0,
                            null);
            var copied = submission.copiedMessagesForTest();
            try {
                CompletableFuture.runAsync(
                                ZLinkBlockingCalls.infrastructureTask(
                                        () -> {
                                            Callable<Object> invoke = submission::submit;
                                            if (rejectTurn) {
                                                assertSame(
                                                        rejected,
                                                        assertThrows(
                                                                RejectedExecutionException.class,
                                                                invoke::call));
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
                assertEquals(2, copied.size());
                for (Message message : copied) {
                    assertThrows(
                            IllegalStateException.class,
                            message::refCount,
                            "a rejected submission must release its copied messages");
                }
                assertEquals("packet", packet.toUtf8String());
                assertEquals("payload", body.toUtf8String());
            } finally {
                Message.closeAll(copied);
                mailbox.clear();
                submissionFinished.set(true);
            }
        } finally {
            lane.closeAsync().toCompletableFuture().join();
        }
    }
}
