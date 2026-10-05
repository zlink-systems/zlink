/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.errors.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

class TargetRemovalResultContractTest {
    @Test
    void writableProjectionAndLaterSubmitKeepTheirCoreResults() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    @Test
    void disconnectRidProjectsNotFoundAsConnectException() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             Poller poller = Zlink.createPoller()) {
            RoutingId expectedRid = RoutingId.from("java-disconnect-result-peer");
            dealer.setRoutingId(expectedRid);
            String endpoint = TestSupport.inprocEndpoint("disconnect-rid-result");
            router.bind(endpoint);
            dealer.connect(endpoint);

            try (Message probe = Message.from("route-prime")) {
                dealer.send().message(probe).submit_sync();
            }
            RoutingId peerRid;
            try (Received received = new Received()) {
                assertTrue(router.recv(received, RecvFlags.NONE));
                peerRid = received.getRoutingId().orElseThrow();
            }
            assertEquals(expectedRid, peerRid);
            try (SocketMonitor monitor = router.monitorOpen(
                     MonitorEventType.DISCONNECTED)) {
                poller.add(monitor, 1, PollEventFlags.POLLIN);
                router.disconnectRid(peerRid);

                PollEvents events = new PollEvents(1);
                assertEquals(1, poller.wait(events,
                    Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
                MonitorEvent disconnected = monitor.recv(RecvFlags.DONT_WAIT);
                assertNotNull(disconnected);
                assertEquals(MonitorEventType.DISCONNECTED,
                    disconnected.event());
                assertEquals(peerRid, disconnected.routingId().orElseThrow());
            }
            ZlinkConnectException error = assertThrows(ZlinkConnectException.class,
                () -> router.disconnectRid(peerRid));
            assertEquals(605, error.getResult().value());
            assertEquals(ConnectResult.NOT_FOUND, error.getResult());
        }
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) router);
            RoutingId rid = RoutingId.from(new byte[] {4, 1});
            for (boolean request : new boolean[] {false, true}) {
                for (int completionResult : new int[] {801, 802, 803, 999, 0}) {
                    boolean terminal = completionResult != 0;
                    int terminalErrno = completionResult == 801
                        ? NativeErrno.ENOENT
                        : completionResult == 803 ? NativeErrno.EAGAIN
                        : NativeErrno.ENOTCONN;
                    core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 41));
                    CompletableFuture<Void> admitted;
                    CompletableFuture<?> waiter;
                    if (request) {
                        RequestSubmission submission = router.request(rid)
                            .message(Message.from("pending"))
                            .timeout(Duration.ofSeconds(2)).submit();
                        assertEquals(SubmitResult.BACKPRESSURED,
                            submission.result());
                        admitted = submission.admitted().toCompletableFuture();
                        waiter = submission.reply().toCompletableFuture();
                    } else {
                        SendSubmission submission = router.send(rid)
                            .message(Message.from("pending")).submit();
                        assertEquals(SubmitResult.BACKPRESSURED,
                            submission.result());
                        admitted = submission.admitted().toCompletableFuture();
                        waiter = admitted;
                    }
                    var pending = core.submissions.getLast();
                    if (!terminal)
                        core.writable(pending, 0, 0);
                    router.disconnectRid(rid);
                    if (terminal)
                        core.writable(pending, completionResult, terminalErrno);
                    else
                        core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.NOT_CONNECTED, NativeErrno.EHOSTUNREACH, 0));
                    assertEquals(1, owner.drain());
                    Throwable failure = CompletionNativeFixture.failure(waiter);
                    assertSame(failure,
                        CompletionNativeFixture.failure(admitted),
                        "admission and reply must expose the same terminal cause");
                    var submit = assertInstanceOf(ZlinkSubmitException.class, failure);
                    assertEquals(completionResult == 801 ? SubmitResult.NOT_FOUND
                        : completionResult == 803 || completionResult == 999 ? SubmitResult.INTERNAL_ERROR
                        : SubmitResult.NOT_CONNECTED, submit.getResult());
                    assertEquals(submit.getResult() == SubmitResult.INTERNAL_ERROR ? NativeErrno.EPROTO
                        : terminal ? terminalErrno : NativeErrno.EHOSTUNREACH,
                        assertInstanceOf(ZlinkException.class, failure).getNativeErrno());
                }
            }
            core.verify(10);
        }
    }
}
