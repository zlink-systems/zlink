package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Connection;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ConnectionRef;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Delivery;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.streams.ZLinkSession;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkAdmissionQueueLeaseFailureTest {
    private static final String NODE = AdmissionQueueFixture.id("node");
    private static final String OTHER_NODE = AdmissionQueueFixture.id("other-node");
    private static final String FIRST = AdmissionQueueFixture.id("first");

    private final IllegalStateException storeFailure =
            new IllegalStateException("lease read failed");
    private final AtomicBoolean failLeaseRead = new AtomicBoolean(true);
    private final ZLinkAdmissionQueueSpot spot = createSpot();
    private final ZLinkAdmissionQueueSpot.ReportHandler reportHandler =
            new ZLinkAdmissionQueueSpot.ReportHandler();
    private final ZLinkAdmissionQueueSpot.EnqueueHandler enqueueHandler =
            new ZLinkAdmissionQueueSpot.EnqueueHandler();
    private final ZLinkAdmissionQueueSpot.AdmitPermitHandler admitHandler =
            new ZLinkAdmissionQueueSpot.AdmitPermitHandler();

    @Test
    void admitAtCapFailsWithoutInvalidatingTheTicket() {
        var request = acquireFirstPermit();
        assertSame(
                storeFailure,
                assertThrows(
                                CompletionException.class,
                                () ->
                                        admitHandler
                                                .handle(spot, request)
                                                .toCompletableFuture()
                                                .join())
                        .getCause());
        failLeaseRead.set(false);
        assertTrue(admitHandler.handle(spot, request).toCompletableFuture().join().granted());
    }

    @Test
    void reportAtCapFailsWithoutDeliveringATicket() {
        acquireFirstPermit();
        reportHandler
                .handle(
                        spot,
                        new NodeReport(
                                NODE,
                                1L,
                                1,
                                0,
                                List.of(new ConnectionRef("login", FIRST)),
                                List.of(),
                                List.of(),
                                0L))
                .toCompletableFuture()
                .join();
        var second = new Connection(OTHER_NODE, 1L, "login", AdmissionQueueFixture.id("second"));
        assertEquals(
                Delivery.POSITION,
                enqueueHandler
                        .handle(spot, new Enqueue(second))
                        .toCompletableFuture()
                        .join()
                        .delivery()
                        .kind());

        var report = report(OTHER_NODE);
        assertSame(
                storeFailure,
                assertThrows(
                                CompletionException.class,
                                () ->
                                        reportHandler
                                                .handle(spot, report)
                                                .toCompletableFuture()
                                                .join())
                        .getCause());
        failLeaseRead.set(false);
        var deliveries =
                reportHandler.handle(spot, report).toCompletableFuture().join().deliveries();
        assertEquals(Delivery.TICKET, deliveries.getFirst().kind());
        assertEquals(second.sessionRid(), deliveries.getFirst().sessionRid());
    }

    private AdmitPermit acquireFirstPermit() {
        var epoch = reportHandler.handle(spot, report(NODE)).toCompletableFuture().join().epoch();
        var first = new Connection(NODE, 1L, "login", FIRST);
        assertEquals(
                Delivery.TICKET,
                enqueueHandler
                        .handle(spot, new Enqueue(first))
                        .toCompletableFuture()
                        .join()
                        .delivery()
                        .kind());
        var request = new AdmitPermit(NODE, 1L, "owner", 1L, "login", FIRST, epoch, 1L);
        assertTrue(admitHandler.handle(spot, request).toCompletableFuture().join().granted());
        return new AdmitPermit(NODE, 1L, "owner", 1L, "login", FIRST, epoch, 2L);
    }

    private static NodeReport report(String node) {
        return new NodeReport(node, 1L, 1, 0, List.of(), List.of(), List.of(), 0L);
    }

    private ZLinkAdmissionQueueSpot createSpot() {
        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("queue").listen(0).objects().server();
        options.addStreamNode("login")
                .bind(0)
                .registerSession(LoginSession.class)
                .authenticationPacketName("Login")
                .admissionQueueEnabled(true)
                .admissionQueueKey(AdmissionQueueFixture.KEY)
                .admissionGlobalLimit(1);
        var registration = options.registration();
        var host =
                ZLinkAdmissionQueueHost.create(
                        registration.streamNodes(),
                        registration.meshNodes(),
                        true,
                        (owner, generation) ->
                                failLeaseRead.get()
                                        ? CompletableFuture.failedFuture(storeFailure)
                                        : CompletableFuture.completedFuture(false));
        return new ZLinkAdmissionQueueSpot(null, host);
    }

    private abstract static class LoginSession implements ZLinkSession {}
}
