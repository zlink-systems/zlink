package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkPendingStreamAdmissionTerminationTest {
    @RepeatedTest(40)
    void socketCloseTerminatesPendingPush() throws Exception {
        exercise(true, Set.of(SubmitResult.TERMINATED));
    }

    @RepeatedTest(40)
    void peerDisconnectTerminatesPendingPush() throws Exception {
        // Core's completion table ends a pending send with NOT_FOUND when the RID is removed
        // and NOT_CONNECTED when the STREAM connection closes; a peer disconnect does both and
        // the spec orders neither. The Framework maps both to Unavailable (Submit and
        // completion §7), so either one ends the push; a wait or any other result fails.
        exercise(false, Set.of(SubmitResult.NOT_FOUND, SubmitResult.NOT_CONNECTED));
    }

    private void exercise(boolean closeSocket, Set<SubmitResult> expected) throws Exception {
        var ready = new CompletableFuture<RoutingId>();
        try (var context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            var socket = context.createStreamSocket();
            socket.options().sendHwm(4096);
            socket.options().sendBuffer(4096);
            socket.options().linger(Duration.ZERO);
            try (var stream =
                            new ZLinkJavaStreamSocket(
                                    socket, null, new ZLinkStateLane(Runnable::run));
                    var client = new Socket()) {
                client.setReceiveBufferSize(4096);
                stream.onTransportError(
                        (rid, event, code, message) -> {
                            if (event == MonitorEventType.CONNECTION_READY) ready.complete(rid);
                        });
                stream.bind("tcp://127.0.0.1:0");
                var endpoint = URI.create(stream.lastEndpoint());
                client.connect(new InetSocketAddress(endpoint.getHost(), endpoint.getPort()));
                var rid = ready.get(5, TimeUnit.SECONDS);
                var pending = submitUntilPending(stream, rid);
                assertFalse(pending.isDone());

                var terminated = new CompletableFuture<Void>();
                var termination =
                        Thread.ofVirtual()
                                .start(
                                        () -> {
                                            try {
                                                if (closeSocket) stream.close();
                                                else stream.disconnectPeer(rid);
                                                terminated.complete(null);
                                            } catch (RuntimeException | Error failure) {
                                                terminated.completeExceptionally(failure);
                                            }
                                        });
                try {
                    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
                    if (!closeSocket) {
                        // Drive the existing receive poller as the STREAM receive loop does.
                        while (!pending.isDone() && System.nanoTime() < deadline) {
                            stream.waitForReadable(Duration.ofMillis(10));
                        }
                    }
                    try {
                        terminated.get(remainingNanos(deadline), TimeUnit.NANOSECONDS);
                    } catch (TimeoutException timeout) {
                        // #4889: show whether close() waits on the state lane or in socket close.
                        fail(
                                "termination waited for a pending STREAM push\n"
                                        + threadStacks(termination),
                                timeout);
                    }
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () ->
                                            pending.get(
                                                    remainingNanos(deadline),
                                                    TimeUnit.NANOSECONDS));
                    var result =
                            assertInstanceOf(ZlinkSubmitException.class, failure.getCause())
                                    .getResult();
                    assertTrue(expected.contains(result), () -> "unexpected result " + result);
                } finally {
                    // Release native backpressure even when the operation under test is stuck.
                    if (!pending.isDone()) socket.close();
                    assertTrue(termination.join(Duration.ofSeconds(5)));
                }
            }
        }
    }

    private static long remainingNanos(long deadline) {
        return Math.max(0, deadline - System.nanoTime());
    }

    private CompletableFuture<Void> submitUntilPending(ZLinkJavaStreamSocket stream, RoutingId rid)
            throws Exception {
        var frame =
                ZLinkStreamFrameCodec.encode(
                        new ZLinkStreamHeader("Push", Map.of(), Optional.empty()), new byte[2048]);
        try (var payload = Message.from(frame)) {
            for (int attempt = 0; attempt < 1024; attempt++) {
                var submitted =
                        stream.sendBoundSessionPushAsync(rid, 1, List.of(payload))
                                .toCompletableFuture();
                try {
                    submitted.get(50, TimeUnit.MILLISECONDS);
                } catch (TimeoutException pending) {
                    return submitted;
                }
            }
        }
        return fail("unread STREAM peer did not cause pending admission");
    }

    /** The terminating virtual thread first, then every platform thread, with full stacks. */
    private static String threadStacks(Thread termination) {
        var dump = new StringBuilder();
        append(dump, termination, termination.getStackTrace());
        Thread.getAllStackTraces().forEach((thread, stack) -> append(dump, thread, stack));
        return dump.toString();
    }

    private static void append(StringBuilder dump, Thread thread, StackTraceElement[] stack) {
        dump.append('"').append(thread.getName()).append("\" ").append(thread.getState());
        dump.append(thread.isVirtual() ? " virtual\n" : "\n");
        for (var frame : stack) dump.append("    at ").append(frame).append('\n');
    }
}
