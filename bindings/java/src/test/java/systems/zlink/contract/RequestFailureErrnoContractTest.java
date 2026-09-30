/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterRoute;
import systems.zlink.contracts.sockets.RouterSocket;

/** Exercises public request failures and their Core representative errnos. */
public class RequestFailureErrnoContractTest {
    private static final int ENOENT = 2;
    private static final int ENOTCONN = 107;
    private static final int ETIMEDOUT = 110;

    @Test
    public void requestTimeoutPreservesRepresentativeErrno() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket();
             ExecutorService worker = Executors.newSingleThreadExecutor();
             Poller poller = Zlink.createPoller();
             var serverMonitor = server.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             var clientMonitor = client.monitorOpen(
                 MonitorEventType.CONNECTION_READY)) {
            String endpoint = TestSupport.inprocEndpoint(
                "request-errno-timeout");
            client.setRoutingId(RoutingId.from("errno-timeout-client"));
            server.bind(endpoint);
            client.connect(endpoint);
            TestSupport.awaitMonitorEvent(serverMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(clientMonitor,
                MonitorEventType.CONNECTION_READY);
            poller.add(server, 1L, PollEventFlags.POLLIN);

            Future<ZlinkRequestException> pending = worker.submit(() ->
                submitDealerFailure(client, Duration.ofMillis(300)));
            awaitReadable(poller);
            try (Received request = new Received()) {
                assertTrue(server.recv(request, RecvFlags.NONE));
                ZlinkRequestException failure = pending.get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertFailure(failure, RequestResult.TIMED_OUT, ETIMEDOUT,
                    "request timeout");
            }
        }
    }

    @Test
    public void removingEndpointCompletesPendingRequestAsNotFound()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket requester = context.createRouterSocket();
             RouterSocket target = context.createRouterSocket();
             ExecutorService worker = Executors.newSingleThreadExecutor();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint(
                "request-errno-endpoint-removal");
            RoutingId targetRid = RoutingId.from("errno-endpoint-target");
            target.setRoutingId(targetRid);
            connectRouters(requester, target, endpoint, targetRid);
            poller.add(target, 1L, PollEventFlags.POLLIN);

            Future<ZlinkRequestException> pending = worker.submit(() ->
                submitRouterFailure(requester, targetRid,
                    Duration.ofSeconds(4)));
            awaitReadable(poller);
            try (Received request = new Received()) {
                assertTrue(target.recv(request, RecvFlags.NONE));
                requester.disconnect(endpoint);
                ZlinkRequestException failure = pending.get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertFailure(failure, RequestResult.NOT_FOUND, ENOENT,
                    "explicit endpoint removal");
            }
        }
    }

    @Test
    public void targetTerminationCompletesPendingRequestAsNotConnected()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket requester = context.createRouterSocket();
             RouterSocket target = context.createRouterSocket();
             ExecutorService worker = Executors.newSingleThreadExecutor();
             Poller poller = Zlink.createPoller()) {
            String endpoint = TestSupport.inprocEndpoint(
                "request-errno-target-termination");
            RoutingId targetRid = RoutingId.from("errno-terminated-target");
            target.options().linger(Duration.ZERO);
            target.setRoutingId(targetRid);
            connectRouters(requester, target, endpoint, targetRid);
            poller.add(target, 1L, PollEventFlags.POLLIN);

            Future<ZlinkRequestException> pending = worker.submit(() ->
                submitRouterFailure(requester, targetRid,
                    Duration.ofSeconds(4)));
            awaitReadable(poller);
            try (Received request = new Received()) {
                assertTrue(target.recv(request, RecvFlags.NONE));
                target.close();
                ZlinkRequestException failure = pending.get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertFailure(failure, RequestResult.NOT_CONNECTED, ENOTCONN,
                    "accepted request target termination");
            }
        }
    }

    private static void connectRouters(RouterSocket requester,
                                       RouterSocket target,
                                       String endpoint,
                                       RoutingId targetRid) {
        try (var requesterMonitor = requester.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             var targetMonitor = target.monitorOpen(
                 MonitorEventType.CONNECTION_READY)) {
            target.bind(endpoint);
            requester.connect(endpoint);
            TestSupport.awaitMonitorEvent(targetMonitor,
                MonitorEventType.CONNECTION_READY);
            TestSupport.awaitMonitorEvent(requesterMonitor,
                MonitorEventType.CONNECTION_READY);
        }
        TestSupport.awaitCondition(() -> requester.routesSnapshot().stream()
            .map(RouterRoute::routingId)
            .anyMatch(targetRid::equals));
    }

    private static void awaitReadable(Poller poller) {
        PollEvents events = new PollEvents(1);
        assertEquals(1, poller.wait(events,
            Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
        assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));
    }

    private static ZlinkRequestException submitDealerFailure(
            DealerSocket client, Duration timeout) {
        try (Message request = Message.from("request")) {
            return captureFailure(() -> client.request()
                .message(request)
                .timeout(timeout)
                .submit_sync());
        }
    }

    private static ZlinkRequestException submitRouterFailure(
            RouterSocket requester, RoutingId target, Duration timeout) {
        try (Message request = Message.from("request")) {
            return captureFailure(() -> requester.request(target)
                .message(request)
                .timeout(timeout)
                .submit_sync());
        }
    }

    private static ZlinkRequestException captureFailure(
            Supplier<List<Message>> submit) {
        try {
            Message.closeAll(submit.get());
        } catch (ZlinkRequestException failure) {
            return failure;
        }
        throw new AssertionError("request unexpectedly returned a reply");
    }

    private static void assertFailure(ZlinkRequestException failure,
                                      RequestResult expectedResult,
                                      int expectedErrno,
                                      String operation) {
        assertEquals(expectedResult, failure.getResult(), operation);
        assertEquals(expectedErrno, failure.getNativeErrno(),
            operation + " native errno");
    }
}
